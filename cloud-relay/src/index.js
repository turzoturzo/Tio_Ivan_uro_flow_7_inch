/**
 * MongoFlo Cloud Relay — Cloudflare Worker
 *
 * Receives CSV POST from ESP32 and forwards to Google Apps Script.
 * Responds immediately to ESP32, forwards to Google in the background.
 */
export default {
  async fetch(request, env, ctx) {
    // CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, {
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "POST, GET",
          "Access-Control-Allow-Headers": "Content-Type",
        },
      });
    }

    if (request.method !== "POST") {
      return new Response(JSON.stringify({ error: "POST required" }), {
        status: 405,
        headers: { "Content-Type": "application/json" },
      });
    }

    // Read CSV body from ESP32
    const body = await request.text();
    if (!body || body.length === 0) {
      return new Response(JSON.stringify({ error: "Empty body" }), {
        status: 400,
        headers: { "Content-Type": "application/json" },
      });
    }

    // Build Google Apps Script URL with query params
    const url = new URL(request.url);
    const targetUrl = new URL(env.GOOGLE_APPS_SCRIPT_URL);
    for (const [key, value] of url.searchParams) {
      targetUrl.searchParams.set(key, value);
    }

    // Fire-and-forget: forward to Google Apps Script in the background.
    // ESP32 gets an immediate 200 response without waiting for Google.
    //
    // Google Apps Script returns a 302 redirect. Per HTTP spec, fetch()
    // with redirect:"follow" converts POST→GET on 302, losing the body.
    // We must manually follow the redirect, re-POSTing the body.
    ctx.waitUntil(
      (async () => {
        try {
          // First request — don't auto-follow redirect
          const res = await fetch(targetUrl.toString(), {
            method: "POST",
            headers: {
              "Content-Type": "text/plain",
              "User-Agent": "MongoFlo-Relay/1.0",
            },
            body: body,
            redirect: "manual",
          });

          // If Google redirected (302/307), re-POST to the Location URL
          if ([301, 302, 303, 307, 308].includes(res.status)) {
            const location = res.headers.get("Location");
            if (location) {
              const res2 = await fetch(location, {
                method: "POST",
                headers: {
                  "Content-Type": "text/plain",
                  "User-Agent": "MongoFlo-Relay/1.0",
                },
                body: body,
                redirect: "follow",
              });
              console.log("GAS final response:", res2.status, await res2.text());
            } else {
              console.error("GAS redirect but no Location header");
            }
          } else {
            console.log("GAS response (no redirect):", res.status, await res.text());
          }
        } catch (err) {
          console.error("GAS forward failed:", err.message);
        }
      })()
    );

    return new Response(
      JSON.stringify({
        status: "ok",
        bytes: body.length,
      }),
      {
        status: 200,
        headers: {
          "Content-Type": "application/json",
          "Access-Control-Allow-Origin": "*",
        },
      }
    );
  },
};
