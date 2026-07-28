import CoreBluetooth
import Foundation

let HR_SERVICE_UUID = CBUUID(string: "180D")
let HR_CHAR_UUID    = CBUUID(string: "2A37")
let LOG_PATH        = "/tmp/whoop_ble.log"

var running = true

func log(_ msg: String) {
    print(msg)
    fflush(stdout)
    if let data = (msg + "\n").data(using: .utf8) {
        if let fh = FileHandle(forWritingAtPath: LOG_PATH) {
            fh.seekToEndOfFile()
            fh.write(data)
            fh.closeFile()
        }
    }
}

class CentralDelegate: NSObject, CBCentralManagerDelegate {
    var manager: CBCentralManager?
    var peripheralDelegate: PeripheralDelegate?

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        manager = central
        switch central.state {
        case .poweredOn:
            log("Bluetooth on — scanning for WHOOP (0x180D)...")
            central.scanForPeripherals(
                withServices: [HR_SERVICE_UUID],
                options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
            )
        case .unauthorized:
            log("FAIL: Bluetooth denied. System Settings → Privacy → Bluetooth → allow.")
            exit(1)
        case .poweredOff:
            log("FAIL: Bluetooth off.")
            exit(1)
        default:
            log("State: \(central.state.rawValue)")
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let name = peripheral.name
            ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? "(unknown)"
        guard name.lowercased().contains("whoop") else { return }

        central.stopScan()
        let advSvcs = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? [])
            .map(\.uuidString).joined(separator: ", ")
        log("Found: \(name)  RSSI=\(RSSI)dBm  advertised=\(advSvcs)")

        // Some BLE HR straps embed the value in service data within the ad
        if let sd = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {
            for (u, d) in sd { log("  service data \(u): \(d as NSData)") }
        }

        log("Connecting (15s timeout)...")
        peripheralDelegate = PeripheralDelegate()
        peripheral.delegate = peripheralDelegate

        DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(15)) {
            if peripheral.state != .connected && running {
                log("FAIL: connect timeout (state=\(peripheral.state.rawValue))")
                log("Try: turn on airplane mode on your phone, then rerun.")
                running = false
            }
        }
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager,
                        didConnect peripheral: CBPeripheral) {
        log("Connected ✓  state=\(peripheral.state.rawValue)")
        log("Max MTU: \(peripheral.maximumWriteValueLength(for: .withResponse))")
        peripheral.discoverServices([HR_SERVICE_UUID])
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        let r = error.map { " — \($0.localizedDescription)" } ?? ""
        log("Disconnected.\(r)")
        running = false
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        let dom = error.map { ($0 as NSError).domain } ?? "?"
        log("FAIL: connect (\(dom)) — \(error?.localizedDescription ?? "?")")
        log("Band is likely already connected to your phone.")
        log("Try: turn on airplane mode on phone, then rerun this tool.")
        running = false
    }
}

class PeripheralDelegate: NSObject, CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverServices error: Error?) {
        if let e = error { log("FAIL: services — \(e.localizedDescription)"); exit(1) }
        let uuids = peripheral.services?.map(\.uuid.uuidString) ?? []
        log("Services: \(uuids.joined(separator: ", "))")
        guard let svc = peripheral.services?.first(where: { $0.uuid == HR_SERVICE_UUID }) else {
            log("FAIL: HR Service (0x180D) not found in services")
            exit(1)
        }
        log("HR Service found ✓")
        peripheral.discoverCharacteristics([HR_CHAR_UUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let e = error { log("FAIL: chars — \(e.localizedDescription)"); exit(1) }
        let uuids = service.characteristics?.map(\.uuid.uuidString) ?? []
        let props = service.characteristics?.map { c in
            "\(c.uuid): \(c.properties.rawValue)"
        } ?? []
        log("Characteristics: \(props.joined(separator: ", "))")
        guard let char = service.characteristics?.first(where: { $0.uuid == HR_CHAR_UUID }) else {
            log("FAIL: HR Measurement char (0x2A37) not found")
            exit(1)
        }
        guard char.properties.contains(.notify) else {
            log("FAIL: HR char does not support notify (props=\(char.properties.rawValue))")
            exit(1)
        }
        log("Subscribing to 0x2A37...")
        peripheral.setNotifyValue(true, for: char)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let e = error {
            log("FAIL: subscribe — \(e.localizedDescription)")
            exit(1)
        }
        log("Subscribed ✓\n\nLive HR:\n")
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard let data = characteristic.value, data.count >= 2 else { return }
        let flags = data[0]
        let contactDetected  = (flags & 0x02) != 0
        let contactSupported = (flags & 0x04) != 0
        let fmt16 = (flags & 0x01) != 0

        let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
        let ts = f.string(from: Date())

        if contactSupported && !contactDetected {
            log("  [\(ts)]  —  (off body)")
            return
        }
        let hr = fmt16
            ? Int(data[1]) | (Int(data[2]) << 8)
            : Int(data[1])
        log("  [\(ts)]  ❤️  \(hr) bpm")
    }
}

try? "".write(toFile: LOG_PATH, atomically: true, encoding: .utf8)

log(String(repeating: "=", count: 48))
log("WHOOP BLE HR broadcast verification")
log(String(repeating: "=", count: 48))
log("\nMake sure HR Broadcast is ON in WHOOP app.\n")

let d = CentralDelegate()
let cm = CBCentralManager(delegate: d, queue: .main)

DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(120)) {
    if running { log("\nTimeout (2 min)."); running = false }
}
while running {
    RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.1))
}
