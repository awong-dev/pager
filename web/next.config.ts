import type { NextConfig } from "next";

// docs/SERVER_PLAN.md §7.1: `output: 'export'` (Firebase Hosting serves
// `web/out/` as a static site + service worker); `next dev` proxies
// `/api/**` and `/webhooks/**` to the relay on :8000. `rewrites()` has no
// effect on the exported build itself (Next prints a one-line warning --
// "rewrites will not automatically work with output: export" -- which is
// expected and harmless: production instead gets its rewrites from
// `web/firebase.json`'s Hosting config, not from this file), but `next dev`
// still honours it, which is the only place this app needs it to.
const nextConfig: NextConfig = {
  output: "export",

  async rewrites() {
    return [
      { source: "/api/:path*", destination: "http://localhost:8000/api/:path*" },
      { source: "/webhooks/:path*", destination: "http://localhost:8000/webhooks/:path*" },
    ];
  },
};

export default nextConfig;
