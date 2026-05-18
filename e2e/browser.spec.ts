import { expect, test } from "@playwright/test";
import { expectCanvasHasContent, joinArena, saveViewportScreenshot } from "./helpers";

test("serves public pages", async ({ page }) => {
  await page.goto("/");
  await expect(page).toHaveTitle("VixArena");
  await expect(page.locator("#arena")).toBeVisible();

  await page.goto("/docs");
  await expect(page.locator("h1")).toHaveText("VixArena");
  await expect(page.getByRole("heading", { name: "WebSocket" })).toBeVisible();

  await page.goto("/stats");
  await expect(page).toHaveTitle(/Stats|VixArena/i);
  await expect(page.locator("body")).toContainText(/Leaderboard|Runtime|Stats/i);
});

test("player can join and canvas renders game state", async ({ page }, testInfo) => {
  await joinArena(page);
  await expectCanvasHasContent(page);
  await expect(page.locator("#score")).toBeVisible();
  await expect(page.locator("#quest")).toBeVisible();
  await expect(page.locator("#roundTime")).toBeVisible();
  await saveViewportScreenshot(page, testInfo, `arena-${testInfo.project.name}`);
});

test("join panel supports room discovery and private room generation", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#roomList")).toBeVisible();
  await expect(page.locator('[data-room-code="public"]')).toBeVisible();

  await page.locator("#newRoomBtn").click();
  await expect(page.locator("#roomInput")).toHaveValue(/^arena-[a-f0-9]{6}$/);

  await page.locator('[data-room-code="public"]').click();
  await expect(page.locator("#roomInput")).toHaveValue("public");

  const rooms = await page.request.get("/api/rooms").then((res) => res.json());
  expect(rooms.service).toBe("vix-arena");
  expect(Array.isArray(rooms.rooms)).toBe(true);
});

test("browser websocket protocol accepts join and returns snapshots", async ({ page }) => {
  await page.goto("/");
  const result = await page.evaluate(async () => {
    const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    const url = `${proto}//${window.location.host}/ws`;
    return new Promise<{ welcome: boolean; snapshot: boolean; pong: boolean }>((resolve, reject) => {
      const ws = new WebSocket(url);
      const seen = { welcome: false, snapshot: false, pong: false };
      const timer = window.setTimeout(() => {
        ws.close();
        reject(new Error(`timed out waiting for websocket messages: ${JSON.stringify(seen)}`));
      }, 8_000);

      ws.addEventListener("open", () => {
        ws.send(JSON.stringify({ type: "join", name: "ProtocolE2E", room: `proto-${Date.now().toString(36)}` }));
        ws.send(JSON.stringify({ type: "ping", t: Date.now() }));
      });

      ws.addEventListener("message", (event) => {
        const msg = JSON.parse(String(event.data));
        if (msg.type === "welcome") seen.welcome = true;
        if (msg.type === "snapshot" || msg.type === "snapshot_delta") seen.snapshot = true;
        if (msg.type === "pong") seen.pong = true;
        if (seen.welcome && seen.snapshot && seen.pong) {
          window.clearTimeout(timer);
          ws.close();
          resolve(seen);
        }
      });

      ws.addEventListener("error", () => {
        window.clearTimeout(timer);
        reject(new Error("websocket error"));
      });
    });
  });

  expect(result).toEqual({ welcome: true, snapshot: true, pong: true });
});
