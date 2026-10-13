
const CACHE_NAME = "ramatech-home-ai-v1";

const APP_FILES = [
  "/",
  "/index.html",
  "/manifest.json",
  "/logo.png",
  "/icons/icon-192.png",
  "/icons/icon-512.png",
  "/icons/apple-touch-icon.png"
];

// Install: cache the core app files.
// Missing optional files won't prevent installation.
self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then(async (cache) => {
        await Promise.all(
          APP_FILES.map(async (file) => {
            try {
              const response = await fetch(file, {
                cache: "reload"
              });

              if (response.ok) {
                await cache.put(file, response);
              }
            } catch (error) {
              console.warn("Could not cache:", file, error);
            }
          })
        );
      })
      .then(() => self.skipWaiting())
  );
});

// Activate: remove older app caches.
self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) =>
        Promise.all(
          keys
            .filter((key) =>
              key.startsWith("ramatech-home-ai-") &&
              key !== CACHE_NAME
            )
            .map((key) => caches.delete(key))
        )
      )
      .then(() => self.clients.claim())
  );
});

// Fetch: network-first for pages,
// cache-first for static same-origin assets.
self.addEventListener("fetch", (event) => {
  const request = event.request;

  if (request.method !== "GET") return;

  const url = new URL(request.url);

  if (url.origin !== self.location.origin) return;

  // Leave dynamic endpoints live.
  if (
    url.pathname === "/status" ||
    url.pathname.startsWith("/api/")
  ) {
    return;
  }

  // Page navigation: network first.
  if (request.mode === "navigate") {
    event.respondWith(
      fetch(request)
        .then((response) => {
          if (response.ok) {
            const copy = response.clone();

            caches.open(CACHE_NAME)
              .then((cache) =>
                cache.put("/index.html", copy)
              )
              .catch((error) =>
                console.warn("Page cache failed:", error)
              );
          }

          return response;
        })
        .catch(async () => {
          return (
            await caches.match(request) ||
            await caches.match("/index.html") ||
            Response.error()
          );
        })
    );

    return;
  }

  // Static assets: cache first.
  event.respondWith(
    caches.match(request)
      .then(async (cached) => {
        if (cached) return cached;

        const response = await fetch(request);

        if (response.ok) {
          const copy = response.clone();

          caches.open(CACHE_NAME)
            .then((cache) => cache.put(request, copy))
            .catch((error) =>
              console.warn("Asset cache failed:", error)
            );
        }

        return response;
      })
      .catch(async () => {
        return (
          await caches.match("/index.html") ||
          Response.error()
        );
      })
  );
});
