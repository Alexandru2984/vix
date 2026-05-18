import { expect, type Locator, type Page, type TestInfo } from "@playwright/test";

export async function joinArena(page: Page, namePrefix = "E2E") {
  await page.goto("/");
  await expect(page.locator("#arena")).toBeVisible();
  await expect(page.locator("#status")).toHaveText(/online|connecting/);
  await expect(page.locator("#status")).toHaveText("online", { timeout: 10_000 });

  const name = `${namePrefix}-${Date.now().toString(36).slice(-6)}`;
  await page.locator("#nameInput").fill(name);
  await page.locator("#roomInput").fill(`qa-${Date.now().toString(36).slice(-5)}`);
  await page.locator("#joinBtn").click();

  await expect(page.locator("#joinPanel")).toBeHidden({ timeout: 10_000 });
  await expect(page.locator("#players")).not.toHaveText("0", { timeout: 10_000 });
  await expect(page.locator("#objectiveLabel")).not.toHaveText("Join arena", { timeout: 10_000 });
  await page.waitForTimeout(700);
  return name;
}

export async function expectCanvasHasContent(page: Page) {
  await expect.poll(async () => {
    return page.locator("#arena").evaluate((canvas) => {
      const c = canvas as HTMLCanvasElement;
      const ctx = c.getContext("2d");
      if (!ctx || c.width < 1 || c.height < 1) return 0;

      const colors = new Set<string>();
      const stepX = Math.max(1, Math.floor(c.width / 24));
      const stepY = Math.max(1, Math.floor(c.height / 16));
      for (let y = Math.floor(stepY / 2); y < c.height; y += stepY) {
        for (let x = Math.floor(stepX / 2); x < c.width; x += stepX) {
          const pixel = ctx.getImageData(x, y, 1, 1).data;
          colors.add(`${pixel[0]},${pixel[1]},${pixel[2]},${pixel[3]}`);
        }
      }
      return colors.size;
    });
  }, { timeout: 10_000 }).toBeGreaterThan(8);
}

export async function saveViewportScreenshot(page: Page, testInfo: TestInfo, name: string) {
  await page.screenshot({
    path: testInfo.outputPath(`${name}.png`),
    fullPage: false
  });
}

export async function expectNoHorizontalOverflow(page: Page) {
  const overflow = await page.evaluate(() => {
    const doc = document.documentElement;
    return {
      scrollWidth: doc.scrollWidth,
      clientWidth: doc.clientWidth,
      bodyScrollWidth: document.body.scrollWidth,
      bodyClientWidth: document.body.clientWidth
    };
  });
  expect(overflow.scrollWidth).toBeLessThanOrEqual(overflow.clientWidth + 2);
  expect(overflow.bodyScrollWidth).toBeLessThanOrEqual(overflow.bodyClientWidth + 2);
}

export async function expectNoOverlap(a: Locator, b: Locator) {
  const boxes = await Promise.all([a.boundingBox(), b.boundingBox()]);
  if (!boxes[0] || !boxes[1]) return;
  const [one, two] = boxes;
  const separated =
    one.x + one.width <= two.x ||
    two.x + two.width <= one.x ||
    one.y + one.height <= two.y ||
    two.y + two.height <= one.y;
  expect(separated, `${await a.evaluate((el) => el.id || el.className)} overlaps ${await b.evaluate((el) => el.id || el.className)}`).toBe(true);
}

export async function expectMinTouchTarget(locator: Locator, minSize = 44) {
  await expect(locator).toBeVisible();
  const box = await locator.boundingBox();
  expect(box).not.toBeNull();
  expect(box!.width).toBeGreaterThanOrEqual(minSize);
  expect(box!.height).toBeGreaterThanOrEqual(minSize);
}
