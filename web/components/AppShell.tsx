"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { type ReactNode, useState } from "react";
import AppBar from "@mui/material/AppBar";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Container from "@mui/material/Container";
import Divider from "@mui/material/Divider";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import Toolbar from "@mui/material/Toolbar";
import Typography from "@mui/material/Typography";
import AdminPanelSettingsIcon from "@mui/icons-material/AdminPanelSettings";
import ChatIcon from "@mui/icons-material/Chat";
import SettingsIcon from "@mui/icons-material/Settings";

import { useAuth } from "@/lib/auth-context";

import NotificationWatcher from "./NotificationWatcher";

const settingsLinks = [
  { href: "/settings/backends", label: "Backends" },
  { href: "/settings/notifications", label: "Notifications" },
  { href: "/settings/devices", label: "My devices" },
];

const adminLinks = [
  { href: "/admin/users", label: "Users" },
  { href: "/admin/allowlist", label: "Allow-list" },
  { href: "/admin/devices", label: "Devices" },
  { href: "/admin/settings", label: "Retention" },
];

function NavMenu({
  label,
  icon,
  links,
}: {
  label: string;
  icon: ReactNode;
  links: { href: string; label: string }[];
}) {
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const pathname = usePathname();
  const isActive = links.some((l) => pathname?.startsWith(l.href));

  return (
    <>
      <Button
        color="inherit"
        startIcon={icon}
        onClick={(e) => setAnchor(e.currentTarget)}
        sx={{ fontWeight: isActive ? 700 : 400 }}
      >
        {label}
      </Button>
      <Menu anchorEl={anchor} open={Boolean(anchor)} onClose={() => setAnchor(null)}>
        {links.map((l) => (
          <MenuItem key={l.href} component={Link} href={l.href} onClick={() => setAnchor(null)}>
            {l.label}
          </MenuItem>
        ))}
      </Menu>
    </>
  );
}

/** Top nav shell for every signed-in page -- docs/SERVER_PLAN.md §7.2's
 * route list, laid out as a plain MUI AppBar (kept deliberately simple per
 * §7.5: "keep it a plain MUI Table/AppBar"). */
export default function AppShell({ children }: { children: ReactNode }) {
  const { me, isAdmin, signOutUser } = useAuth();

  return (
    <Box sx={{ display: "flex", flexDirection: "column", minHeight: "100vh" }}>
      <NotificationWatcher />
      <AppBar position="static" color="primary" enableColorOnDark>
        <Toolbar sx={{ gap: 1 }}>
          <Typography
            variant="h6"
            component={Link}
            href="/chat"
            sx={{ color: "inherit", textDecoration: "none", mr: 2 }}
          >
            Pager
          </Typography>
          <Button color="inherit" component={Link} href="/chat" startIcon={<ChatIcon />}>
            Chat
          </Button>
          <NavMenu label="Settings" icon={<SettingsIcon />} links={settingsLinks} />
          {isAdmin && (
            <NavMenu label="Admin" icon={<AdminPanelSettingsIcon />} links={adminLinks} />
          )}
          <Box sx={{ flexGrow: 1 }} />
          {me && (
            <Typography variant="body2" sx={{ opacity: 0.85 }}>
              {me.displayName} (@{me.alias})
            </Typography>
          )}
          <Button color="inherit" onClick={() => void signOutUser()}>
            Sign out
          </Button>
        </Toolbar>
      </AppBar>
      <Divider />
      <Container maxWidth="md" sx={{ flexGrow: 1, py: 3 }}>
        {children}
      </Container>
    </Box>
  );
}
