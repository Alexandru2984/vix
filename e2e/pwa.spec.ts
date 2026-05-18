import { expect, test } from "@playwright/test";

test("serves installable PWA metadata and icons", async ({ page, baseURL }) => {
  await page.goto("/");

  await expect(page.locator('link[rel="manifest"]')).toHaveAttribute("href", "/manifest.json");
  await expect(page.locator('meta[name="theme-color"]')).toHaveAttribute("content", "#090b10");

  const manifestResponse = await page.request.get(`${baseURL}/manifest.json`);
  expect(manifestResponse.ok()).toBe(true);
  expect(manifestResponse.headers()["content-type"]).toContain("application/json");

  const manifest = await manifestResponse.json();
  expect(manifest.name).toBe("VixArena");
  expect(manifest.short_name).toBe("VixArena");
  expect(manifest.display).toBe("standalone");
  expect(manifest.start_url).toContain("/");
  expect(manifest.icons).toEqual(expect.arrayContaining([
    expect.objectContaining({ src: "/icons/icon.svg", purpose: "any" }),
    expect.objectContaining({ src: "/icons/maskable.svg", purpose: "maskable" })
  ]));

  for (const icon of manifest.icons) {
    const iconResponse = await page.request.get(`${baseURL}${icon.src}`);
    expect(iconResponse.ok()).toBe(true);
    expect(iconResponse.headers()["content-type"]).toContain("image/svg+xml");
  }
});

test("registers service worker without caching realtime endpoints", async ({ page, baseURL }) => {
  await page.goto("/");

  const registrationState = await page.waitForFunction(async () => {
    if (!("serviceWorker" in navigator)) return "unsupported";
    const registration = await navigator.serviceWorker.getRegistration("/");
    return registration?.active?.state || registration?.installing?.state || registration?.waiting?.state || "";
  }, null, { timeout: 10_000 });
  expect(await registrationState.jsonValue()).toMatch(/activated|activating|installed|installing/);

  const swResponse = await page.request.get(`${baseURL}/sw.js`);
  expect(swResponse.ok()).toBe(true);
  expect(swResponse.headers()["cache-control"]).toContain("no-store");
  const swSource = await swResponse.text();
  expect(swSource).toContain('url.pathname === "/ws"');
  expect(swSource).toContain('url.pathname.startsWith("/api/")');
  expect(swSource).toContain('url.pathname === "/metrics"');
});
