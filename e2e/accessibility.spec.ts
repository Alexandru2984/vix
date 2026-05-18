import { AxeBuilder } from "@axe-core/playwright";
import { expect, test } from "@playwright/test";
import { expectMinTouchTarget, joinArena } from "./helpers";

test("core pages pass automated accessibility checks", async ({ page }) => {
  for (const path of ["/", "/docs", "/stats"]) {
    await page.goto(path);
    const results = await new AxeBuilder({ page })
      .disableRules(["color-contrast"])
      .analyze();
    expect(results.violations).toEqual([]);
  }
});

test("keyboard focus is visible on interactive controls", async ({ page }) => {
  await page.goto("/");
  await page.keyboard.press("Tab");
  const focused = page.locator(":focus");
  await expect(focused).toBeVisible();

  const focusOutline = await focused.evaluate((el) => {
    const styles = getComputedStyle(el);
    return {
      outlineStyle: styles.outlineStyle,
      outlineWidth: styles.outlineWidth,
      boxShadow: styles.boxShadow,
      borderColor: styles.borderColor
    };
  });

  const hasVisibleFocus =
    focusOutline.outlineStyle !== "none" ||
    focusOutline.outlineWidth !== "0px" ||
    focusOutline.boxShadow !== "none" ||
    focusOutline.borderColor !== "rgba(160, 180, 210, 0.18)";
  expect(hasVisibleFocus).toBe(true);
});

test("touch controls meet minimum practical target size", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name === "desktop", "mobile-only touch target checks");

  await joinArena(page, "A11y");
  await expectMinTouchTarget(page.locator("#mobileInfoBtn"));
  await expectMinTouchTarget(page.locator("#mobileChatBtn"));
  await expectMinTouchTarget(page.locator("#dashBtn"));
  await expectMinTouchTarget(page.locator("#shieldBtn"));
  await expectMinTouchTarget(page.locator("#magnetBtn"));
  await expectMinTouchTarget(page.locator("#touchStick"), 92);
});
