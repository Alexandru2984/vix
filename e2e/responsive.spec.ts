import { expect, test } from "@playwright/test";
import {
  expectMinTouchTarget,
  expectNoHorizontalOverflow,
  expectNoOverlap,
  joinArena,
  saveViewportScreenshot
} from "./helpers";

test("arena layout has no horizontal overflow after join", async ({ page }, testInfo) => {
  await joinArena(page, "Layout");
  await expectNoHorizontalOverflow(page);
  await saveViewportScreenshot(page, testInfo, `layout-${testInfo.project.name}`);
});

test("mobile HUD, chat, abilities, and joystick fit without collisions", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name === "desktop", "mobile-only layout checks");

  await joinArena(page, "Mobile");
  await expect(page.locator("#touchStick")).toBeVisible();
  await expect(page.locator(".mobile-actions")).toBeVisible();
  await expect(page.locator(".ability-bar")).toBeVisible();
  await expect(page.locator(".objective-hud")).toBeVisible();

  await expectMinTouchTarget(page.locator("#touchStick"), 92);
  await expectMinTouchTarget(page.locator("#mobileChatBtn"));
  await expectMinTouchTarget(page.locator("#mobileInfoBtn"));
  await expectMinTouchTarget(page.locator("#dashBtn"));
  await expectMinTouchTarget(page.locator("#shieldBtn"));
  await expectMinTouchTarget(page.locator("#magnetBtn"));

  await expectNoOverlap(page.locator("#touchStick"), page.locator(".ability-bar"));
  await expectNoOverlap(page.locator(".mobile-actions"), page.locator(".ability-bar"));

  await page.locator("#mobileChatBtn").click();
  await expect(page.locator(".chat-panel")).toBeVisible();
  await expect(page.locator("#chatInput")).toBeFocused();
  await expect(page.locator(".ability-bar")).toBeHidden();
  await expect(page.locator("#touchStick")).toBeHidden();

  await page.keyboard.press("Escape");
  await expect(page.locator(".chat-panel")).toBeHidden({ timeout: 10_000 });
  await saveViewportScreenshot(page, testInfo, `mobile-chat-${testInfo.project.name}`);
});

test("touch joystick responds to pointer gestures", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name === "desktop", "touch-only layout checks");

  await joinArena(page, "Touch");
  const stick = page.locator("#touchStick");
  const knob = page.locator("#touchKnob");
  const box = await stick.boundingBox();
  expect(box).not.toBeNull();

  const startX = box!.x + box!.width / 2;
  const startY = box!.y + box!.height / 2;
  const movedX = Math.min(startX + 36, box!.x + box!.width - 8);
  const movedY = startY;

  await stick.dispatchEvent("pointerdown", {
    pointerId: 77,
    pointerType: "touch",
    isPrimary: true,
    clientX: startX,
    clientY: startY
  });
  await page.evaluate(({ x, y }) => {
    window.dispatchEvent(new PointerEvent("pointermove", {
      pointerId: 77,
      pointerType: "touch",
      isPrimary: true,
      clientX: x,
      clientY: y,
      bubbles: true
    }));
  }, { x: movedX, y: movedY });

  await expect.poll(async () => knob.evaluate((el) => (el as HTMLElement).style.transform)).toContain("calc");

  await page.evaluate(({ x, y }) => {
    window.dispatchEvent(new PointerEvent("pointerup", {
      pointerId: 77,
      pointerType: "touch",
      isPrimary: true,
      clientX: x,
      clientY: y,
      bubbles: true
    }));
  }, { x: movedX, y: movedY });
  await expect.poll(async () => knob.evaluate((el) => (el as HTMLElement).style.transform)).toBe("translate(-50%, -50%)");
});
