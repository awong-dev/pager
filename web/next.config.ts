import type { NextConfig } from "next";

// docs/SERVER_PLAN.md §7.1: `output: 'export'` (Firebase Hosting serves
// `web/out/` as a static site + service worker); `next dev` proxies
// `/api/**` and `/webhooks/**` to the relay on :8000. `rewrites()` has no
// effect on the exported build itself (Next prints a one-line warning --
// "rewrites will not automatically work with output: export" -- which is
// expected and harmless: production instead gets its rewrites from
// `web/firebase.json`'s Hosting config, not from this file), but `next dev`
// still honours it, which is the only place this app needs it to.
// `output: 'export'` is a *production* constraint, so it is applied only to
// a real build. Under `next dev` it breaks `/chat/[alias]`: that route is
// exported as one placeholder shell (`/chat/_`, see its
// `generateStaticParams`) because export cannot pre-render a page per
// arbitrary future alias, and Firebase Hosting rewrites `/chat/**` onto that
// shell in production (`web/firebase.json`). `next dev` has no such host in
// front of it, so it routes `/chat/kid` straight at `[alias]` and export's
// "every param must be pre-declared" rule rejects it:
//   Page "/chat/[alias]/page" is missing param "/chat/[alias]" in
//   "generateStaticParams()", which is required with "output: export".
// A rewrite does not help -- Next takes the param from the request URL, not
// the rewrite destination. Letting dev render `[alias]` dynamically is
// faithful to production anyway: it renders the same `ThreadPageClient`,
// which reads the alias from `usePathname()` rather than from route params
// precisely so one shell serves every alias. The trade is that dev no longer
// rejects export-incompatible code on the spot; `npm run build` still does.
const isDev = process.env.NODE_ENV === "development";

const nextConfig: NextConfig = {
  ...(isDev ? {} : { output: "export" as const }),

  async rewrites() {
    return {
      // The dev-server stand-in for `web/firebase.json`'s
      // `/chat/** -> /chat/_.html` Hosting rewrite. `beforeFiles` is
      // required: `/chat/[alias]` is a real filesystem route, so an
      // `afterFiles` rewrite (the bare-array default) would never fire.
      // `:alias` matches a single segment, so `/chat` itself is untouched.
      beforeFiles: [{ source: "/chat/:alias", destination: "/chat/_" }],
      afterFiles: [
        { source: "/api/:path*", destination: "http://localhost:8000/api/:path*" },
        { source: "/webhooks/:path*", destination: "http://localhost:8000/webhooks/:path*" },
      ],
    };
  },
};

export default nextConfig;
