import SwiftUI

@main
struct FSDControllerApp: App {
    @StateObject private var controller = BLEController()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(controller)
        }
    }
}
