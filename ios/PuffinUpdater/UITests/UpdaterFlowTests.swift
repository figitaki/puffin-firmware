import XCTest

final class UpdaterFlowTests: XCTestCase {
    // Exercises real controls against an isolated preview, never Bluetooth.
    func testUpdateFlowAndCodeValidation() {
        let app = XCUIApplication()
        app.launchArguments = ["--preview-updater", "idle"]
        app.launch()
        let action = app.buttons["primary-action"]
        XCTAssertTrue(action.waitForExistence(timeout: 5))
        if !action.isHittable { app.swipeUp() }
        action.tap() // The center of the full-width button must be tappable.
        let device = app.buttons["connect-device"]
        XCTAssertTrue(device.waitForExistence(timeout: 3))
        device.tap()
        let code = app.textFields["board-pin"]
        XCTAssertTrue(code.waitForExistence(timeout: 3))
        XCTAssertFalse(action.isEnabled)
        code.tap(); code.typeText("12345")
        XCTAssertFalse(action.isEnabled)
        code.typeText("6")
        XCTAssertTrue(action.isEnabled)
        XCTAssertTrue(action.isHittable)
        action.tap()
        let completion = app.buttons["Preview completion"]
        XCTAssertTrue(completion.waitForExistence(timeout: 3))
        completion.tap()
        XCTAssertTrue(app.staticTexts["Transfer verified"].waitForExistence(timeout: 3))
        action.tap()
        XCTAssertTrue(app.staticTexts["WHAT’S NEW"].waitForExistence(timeout: 3))
    }
}
