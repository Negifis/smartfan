// SmartFan menu bar app: shows what the smartfan service is doing and switches
// its mode. Reads /Library/Logs/SmartFan/status.json, writes the mode file that
// the service reads every second.

import AppKit

struct Status: Decodable {
    struct Fan: Decodable { let target, actual, min, max: Double }
    struct Sensor: Decodable {
        let key: String
        let temp, load, lo, hi: Double  // temp is smoothed: that's what drives the fans
        let raw: Double?  // current reading
        let desc: String
        var now: Double { raw ?? temp }
    }
    let time: Double
    let mode: String
    let controlling: Bool
    let demand: Double
    let limit: Bool
    let leader: String
    let fans: [Fan]
    let sensors: [Sensor]
    var hottest: Sensor? { sensors.max { $0.now < $1.now } }
}

let statusURL = URL(fileURLWithPath: "/Library/Logs/SmartFan/status.json")
let supportDir = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Application Support/SmartFan")
let modeURL = supportDir.appendingPathComponent("mode")
let configURL = supportDir.appendingPathComponent("sensors.conf")

let modes: [(id: String, title: String, hint: String)] = [
    ("smart", "Умный", "по самому горячему датчику"),
    ("quiet", "Тихий", "все пороги на 6°C выше"),
    ("max", "Максимум", "вентиляторы на полную"),
    ("apple", "Apple (авто)", "управляет macOS"),
]

final class App: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let menu = NSMenu()
    var status: Status?

    func applicationDidFinishLaunching(_ note: Notification) {
        menu.delegate = self
        item.menu = menu
        refresh()
        let timer = Timer(timeInterval: 2, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(timer, forMode: .common)  // keep updating while the menu is open
    }

    var isStale: Bool {
        guard let s = status else { return true }
        return Date().timeIntervalSince1970 - s.time > 10
    }

    func refresh() {
        status = (try? Data(contentsOf: statusURL)).flatMap { try? JSONDecoder().decode(Status.self, from: $0) }
        updateButton()
        rebuildMenu()  // NSMenu shows changes live while open
    }

    func updateButton() {
        guard let button = item.button else { return }
        let symbol: String
        var color = NSColor.labelColor
        var text: String
        if isStale {
            symbol = "exclamationmark.triangle"
            color = .systemRed
            text = "SmartFan"
        } else if let s = status {
            symbol = s.mode == "apple" ? "fan" : "fan.fill"
            if s.limit { color = .systemRed } else if s.demand >= 0.7 { color = .systemOrange }
            let rpm = s.fans.map(\.actual).reduce(0, +) / Double(max(s.fans.count, 1))
            text = String(format: "%.0f° %.0f", s.hottest?.now ?? 0, rpm)
        } else { return }
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "SmartFan")
        image?.isTemplate = color == .labelColor
        button.image = color == .labelColor ? image : image?.tinted(color)
        button.imagePosition = .imageLeading
        button.attributedTitle = NSAttributedString(string: " " + text, attributes: [
            .font: NSFont.monospacedDigitSystemFont(ofSize: NSFont.systemFontSize - 1, weight: .regular),
            .foregroundColor: color,
        ])
    }

    func menuNeedsUpdate(_ menu: NSMenu) { rebuildMenu() }

    // Menu content as data; applied to existing NSMenuItems in place so open
    // menus and submenus don't flicker on every refresh.
    indirect enum Row {
        case info(String, NSFont, String?)
        case action(String, Selector, String?, Bool, String)  // title, action, mode id, checked, key
        case submenu(String, [Row])
        case separator

        var shape: String {
            switch self {
            case .info: return "i"
            case .action: return "a"
            case .separator: return "-"
            case .submenu(_, let rows): return "s(" + rows.map(\.shape).joined() + ")"
            }
        }
    }

    static let bold = NSFont.boldSystemFont(ofSize: 0)
    static let plain = NSFont.menuFont(ofSize: 0)
    static let mono = NSFont.monospacedDigitSystemFont(ofSize: 0, weight: .regular)

    // The service shifts thresholds in quiet mode but reports them unshifted.
    func limits(_ x: Status.Sensor) -> (Double, Double) {
        guard status?.mode == "quiet" else { return (x.lo, x.hi) }
        let hi = min(x.hi + 6, 98)
        return (min(x.lo + 6, hi - 3), hi)
    }

    func sensorRow(_ x: Status.Sensor) -> Row {
        let (lo, hi) = limits(x)
        return .info(String(format: "%5.1f°  %3.0f%%   %@", x.now, x.load * 100, x.desc), App.mono,
                     String(format: "%@: порог %.0f→%.0f°C", x.key, lo, hi))
    }

    func rows() -> [Row] {
        var r: [Row] = []
        if isStale {
            r.append(.info("Служба SmartFan не отвечает", App.bold, nil))
            r.append(.info("Вентиляторами управляет macOS", App.plain, nil))
        } else if let s = status {
            let head = s.mode == "apple" ? "Вентиляторами управляет macOS"
                : String(format: "Вентиляторы: %.0f%%%@", s.demand * 100, s.limit ? " — предел" : "")
            r.append(.info(head, App.bold, nil))
            if let h = s.hottest {
                r.append(.info(String(format: "Самая горячая точка: %@, %.1f°C", h.desc, h.now), App.plain, nil))
            }
            if let l = s.sensors.first(where: { $0.key == s.leader }) {
                let (lo, hi) = limits(l)
                r.append(.info(String(format: "Ведущий датчик: %@, %.1f°C (порог %.0f→%.0f)", l.desc, l.now, lo, hi),
                               App.plain, "Датчик, который ближе всех к своему порогу и задаёт обороты"))
            }
            for (i, f) in s.fans.enumerated() {
                r.append(.info(String(format: "Вентилятор %d: %.0f об/мин (цель %.0f)", i + 1, f.actual, f.target),
                               App.plain, nil))
            }
            r.append(.separator)
            r.append(.info("Ближе всего к своему порогу:", App.plain, nil))
            r += s.sensors.prefix(8).map(sensorRow)
            // Stable order (by key groups components together) so lines don't jump around.
            r.append(.submenu("Все датчики (\(s.sensors.count))", s.sensors.sorted { $0.key < $1.key }.map(sensorRow)))
        }
        r.append(.separator)
        r.append(.info("Режим", App.plain, nil))
        let current = status?.mode ?? "smart"
        for m in modes {
            r.append(.action("\(m.title) — \(m.hint)", #selector(setMode(_:)), m.id, m.id == current, ""))
        }
        r.append(.separator)
        r.append(.action("Пороги датчиков…", #selector(openConfig), nil, false, ","))
        r.append(.action("Скрыть значок", #selector(NSApplication.terminate(_:)), nil, false, "q"))
        return r
    }

    func makeItem(_ row: Row) -> NSMenuItem {
        switch row {
        case .separator:
            return .separator()
        case .submenu(_, let children):
            let mi = NSMenuItem(title: "", action: nil, keyEquivalent: "")
            let sub = NSMenu()
            children.forEach { sub.addItem(makeItem($0)) }
            mi.submenu = sub
            update(mi, row)
            return mi
        case .action(_, let sel, _, _, let key):
            let mi = NSMenuItem(title: "", action: sel, keyEquivalent: key)
            mi.target = sel == #selector(NSApplication.terminate(_:)) ? NSApp : self
            update(mi, row)
            return mi
        case .info:
            let mi = NSMenuItem(title: "", action: nil, keyEquivalent: "")
            mi.isEnabled = false
            update(mi, row)
            return mi
        }
    }

    func update(_ mi: NSMenuItem, _ row: Row) {
        switch row {
        case .separator:
            break
        case .info(let title, let font, let tip):
            if mi.attributedTitle?.string != title || mi.attributedTitle?.attribute(.font, at: 0, effectiveRange: nil) as? NSFont != font {
                mi.attributedTitle = NSAttributedString(string: title, attributes: [.font: font])
            }
            if mi.toolTip != tip { mi.toolTip = tip }
        case .action(let title, _, let id, let checked, _):
            if mi.title != title { mi.title = title }
            mi.representedObject = id
            let state: NSControl.StateValue = checked ? .on : .off
            if mi.state != state { mi.state = state }
        case .submenu(let title, let children):
            if mi.title != title { mi.title = title }
            if let sub = mi.submenu {
                for (child, r) in zip(sub.items, children) { update(child, r) }
            }
        }
    }

    var shape = ""

    func rebuildMenu() {
        let r = rows()
        let newShape = r.map(\.shape).joined()
        if newShape == shape {
            for (mi, row) in zip(menu.items, r) { update(mi, row) }
            return
        }
        shape = newShape
        menu.removeAllItems()
        r.forEach { menu.addItem(makeItem($0)) }
    }

    @objc func setMode(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        try? (id + "\n").write(to: modeURL, atomically: true, encoding: .utf8)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { self.refresh() }
    }

    @objc func openConfig() {
        NSWorkspace.shared.open([configURL], withApplicationAt: URL(fileURLWithPath: "/System/Applications/TextEdit.app"),
                                configuration: NSWorkspace.OpenConfiguration())
    }
}

extension NSImage {
    func tinted(_ color: NSColor) -> NSImage {
        let img = copy() as! NSImage
        img.lockFocus()
        color.set()
        NSRect(origin: .zero, size: img.size).fill(using: .sourceAtop)
        img.unlockFocus()
        img.isTemplate = false
        return img
    }
}

let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
