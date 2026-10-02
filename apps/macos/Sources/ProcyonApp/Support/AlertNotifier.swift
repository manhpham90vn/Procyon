import ProcyonKit
import UserNotifications

/// Shows alerts as macOS notifications, also while Procyon is in front.
final class AlertNotifier: NSObject, UNUserNotificationCenterDelegate, @unchecked Sendable {
    static let shared = AlertNotifier()

    private var center: UNUserNotificationCenter? {
        // Notifications need an app bundle; `swift run` has none.
        Bundle.main.bundleIdentifier == nil ? nil : UNUserNotificationCenter.current()
    }

    func install() { center?.delegate = self }

    /// Asks once; macOS remembers the answer. False when the user turned notifications off.
    func requestPermission() async -> Bool {
        guard let center else { return false }
        return (try? await center.requestAuthorization(options: [.alert, .sound])) ?? false
    }

    func post(_ event: AlertEvent) {
        let content = UNMutableNotificationContent()
        content.title = event.title
        content.body = event.message
        content.sound = .default
        content.threadIdentifier = event.kind.rawValue
        center?.add(UNNotificationRequest(identifier: event.id.uuidString, content: content, trigger: nil))
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter, willPresent notification: UNNotification
    ) async -> UNNotificationPresentationOptions {
        [.banner, .sound, .list]
    }
}
