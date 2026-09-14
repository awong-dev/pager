import type { Metadata, Viewport } from "next";

import Providers from "@/components/Providers";

import "./globals.css";

export const metadata: Metadata = {
  title: "Pager",
  description: "School pickup pager -- web app",
  manifest: "/manifest.webmanifest",
  icons: {
    icon: "/icons/icon-192.png",
    apple: "/icons/icon-192.png",
  },
};

export const viewport: Viewport = {
  themeColor: "#2f5d8a",
  width: "device-width",
  initialScale: 1,
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        <Providers>{children}</Providers>
      </body>
    </html>
  );
}
