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
  await expect(page.locator("#roundSummary")).toBeHidden();
  await saveViewportScreenshot(page, testInfo, `arena-${testInfo.project.name}`);
});

test("client performance HUD reports realtime render and network rates", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop", "desktop-only performance HUD check");

  await joinArena(page, "Perf");
  await expect(page.locator("#fps")).not.toHaveText("--", { timeout: 5_000 });
  await expect(page.locator("#snapshotRate")).not.toHaveText("--", { timeout: 5_000 });
  await expect(page.locator("#effectsBtn")).toHaveText("FX on");
  await page.locator("#effectsBtn").click();
  await expect(page.locator("#effectsBtn")).toHaveText("FX low");
  await expect(page.locator("#effectsBtn")).toHaveAttribute("aria-pressed", "false");
});

test("join panel supports room discovery and private room generation", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#roomList")).toBeVisible();
  await expect(page.locator('[data-room-code="public"]')).toBeVisible();
  await expect(page.locator(".lobby-leaderboard")).toBeVisible();
  await expect(page.locator("#lobbyLeaderboardMeta")).toHaveText("public");

  await page.locator("#newRoomBtn").click();
  await expect(page.locator("#roomInput")).toHaveValue(/^arena-[a-f0-9]{6}$/);
  await expect(page.locator("#lobbyLeaderboardMeta")).toHaveText(/^arena-[a-f0-9]{6}$/);

  await page.locator('[data-room-code="public"]').click();
  await expect(page.locator("#roomInput")).toHaveValue("public");
  await expect(page.locator("#lobbyLeaderboardMeta")).toHaveText("public");

  const rooms = await page.request.get("/api/rooms").then((res) => res.json());
  expect(rooms.service).toBe("vix-arena");
  expect(Array.isArray(rooms.rooms)).toBe(true);
  const leaderboard = await page.request.get("/api/leaderboard?room=public").then((res) => res.json());
  expect(leaderboard.room).toBe("public");
  expect(Array.isArray(leaderboard.entries)).toBe(true);
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
