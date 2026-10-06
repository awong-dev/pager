"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { type ReactNode, useEffect, useState } from "react";
import { collection, onSnapshot, query, where } from "firebase/firestore";
import AppBar from "@mui/material/AppBar";
import Badge from "@mui/material/Badge";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import Chip from "@mui/material/Chip";
import Container from "@mui/material/Container";
import Divider from "@mui/material/Divider";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import Toolbar from "@mui/material/Toolbar";
import Typography from "@mui/material/Typography";
import AdminPanelSettingsIcon from "@mui/icons-material/AdminPanelSettings";
import ChatIcon from "@mui/icons-material/Chat";
import FamilyRestroomIcon from "@mui/icons-material/FamilyRestroom";
import LocationOnIcon from "@mui/icons-material/LocationOn";
import SettingsIcon from "@mui/icons-material/Settings";

import { useAuth } from "@/lib/auth-context";
import { useFamily } from "@/lib/family-context";
import { getFirestoreDb } from "@/lib/firebase";
import { useLocatableDevices } from "@/lib/locatableDevices";

import NotificationWatcher from "./NotificationWatcher";

const settingsLinks = [
  { href: "/settings/book", label: "Address book" },
  { href: "/settings/backends", label: "Backends" },
  { href: "/settings/notifications", label: "Notifications" },
  { href: "/settings/devices", label: "My devices" },
];

// docs/FAMILIES_DESIGN.md §5.1 nav table -- family admin surface, backed by
// `/api/family/*` (relay tasks 1.3, 3.x, 4.x). Pages land in later tasks
// (1.8/1.9/3.4/3.6/4.4); linking to them now is fine -- `next build` does
// not fail on links to routes that don't exist yet.
const familyLinks = [
  { href: "/family/people", label: "People" },
  { href: "/family/devices", label: "Devices" },
  { href: "/family/contacts", label: "Contacts" },
  { href: "/family/alerts", label: "Alerts" },
];

// Superadmin surface, global across every family.
const adminLinks = [
  { href: "/admin/families", label: "Families" },
  { href: "/admin/users", label: "All users" },
  { href: "/admin/devices", label: "All devices" },
  { href: "/admin/allowlist", label: "Allow-list" },
  { href: "/admin/settings", label: "Retention" },
];

function NavMenu({
  label,
  icon,
  links,
  badgeCount,
}: {
  label: string;
  icon: ReactNode;
  links: { href: string; label: string }[];
  /** docs/FAMILIES_TASKS.md 4.4: open-alert count, shown on the menu button
   * itself and on the matching `Alerts` item. `undefined`/`0` renders no
   * badge at all. */
  badgeCount?: number;
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
        <Badge color="error" badgeContent={badgeCount} sx={{ "& .MuiBadge-badge": { right: -8, top: -2 } }}>
          {label}
        </Badge>
      </Button>
      <Menu anchorEl={anchor} open={Boolean(anchor)} onClose={() => setAnchor(null)}>
        {links.map((l) => (
          <MenuItem key={l.href} component={Link} href={l.href} onClick={() => setAnchor(null)}>
            {l.href === "/family/alerts" ? (
              <Badge
                color="error"
                badgeContent={badgeCount}
                sx={{ "& .MuiBadge-badge": { right: -12 } }}
              >
                {l.label}
              </Badge>
            ) : (
              l.label
            )}
          </MenuItem>
        ))}
      </Menu>
    </>
  );
}

/** The "Family: Wong ▾" chip in the AppBar -- docs/FAMILIES_DESIGN.md §5.1,
 * super only. Lists every family from `useFamily()` (a live Firestore
 * listener); picking one calls `setFamilyId`, which every `/family/*` page
 * and `/api/family/*` call (via `familyQuery()`) then scopes to. */
function FamilySwitcher() {
  const { family, families, setFamilyId } = useFamily();
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);

  if (families.length === 0) {
    return null;
  }

  return (
    <>
      <Chip
        color="secondary"
        label={`Family: ${family?.name ?? "choose…"} ▾`}
        onClick={(e) => setAnchor(e.currentTarget)}
        sx={{ cursor: "pointer" }}
      />
      <Menu anchorEl={anchor} open={Boolean(anchor)} onClose={() => setAnchor(null)}>
        {families.map((f) => (
          <MenuItem
            key={f.id}
            selected={f.id === family?.id}
            onClick={() => {
              setFamilyId(f.id);
              setAnchor(null);
            }}
          >
            {f.name}
          </MenuItem>
        ))}
      </Menu>
    </>
  );
}

/** Live count of `families/{fam}/alerts where status == 'open'` --
 * docs/FAMILIES_TASKS.md 4.4's badge on the Family menu and the Alerts item.
 * `firestore.rules` (task 1.4) gates the `alerts` subcollection to super or
 * that family's admin, matching `isFamilyAdmin` below. */
function useOpenAlertCount(familyId: string | null, enabled: boolean): number {
  const [count, setCount] = useState(0);

  useEffect(() => {
    // No family in scope (not a family admin, or a super who hasn't picked
    // one yet) -- leave the count at whatever it last was rather than
    // resetting synchronously in the effect body, same
    // `react-hooks/set-state-in-effect` constraint `lib/family-context.tsx`'s
    // admin-only `families` listener sidesteps the same way. Harmless: a
    // stale count from a different scope is replaced the moment this
    // effect's own listener below fires for the new scope, and nothing
    // renders it while `!enabled`.
    if (!enabled || !familyId) {
      return;
    }
    const db = getFirestoreDb();
    const q = query(
      collection(db, "families", familyId, "alerts"),
      where("status", "==", "open")
    );
    const unsubscribe = onSnapshot(q, (snap) => setCount(snap.size));
    return unsubscribe;
  }, [familyId, enabled]);

  return count;
}

/** Top nav shell for every signed-in page -- docs/SERVER_PLAN.md §7.2's
 * route list, laid out as a plain MUI AppBar (kept deliberately simple per
 * §7.5: "keep it a plain MUI Table/AppBar"). Nav items by role per
 * docs/FAMILIES_DESIGN.md §5.1: member gets Chat/Location/Settings; a
 * family admin (`role === 'admin'`) additionally gets the Family menu;
 * super additionally gets the Admin menu and the family switcher. */
export default function AppShell({ children }: { children: ReactNode }) {
  const { me, isFamilyAdmin, isSuper, signOutUser } = useAuth();
  const { familyId } = useFamily();
  const openAlertCount = useOpenAlertCount(familyId, isFamilyAdmin);
  // Nav-visibility only (per this file's own docstring: "the client is not
  // the gate") -- `lib/locatableDevices.ts`'s `devices` mirrors exactly what
  // `firestore.rules` will actually let this account read (own devices,
  // `locatableBy` grants, and every family device for an admin), so "at
  // least one locatable device" here can never show the link to someone who
  // then hits a wall on `/location`. A family admin gets the link
  // unconditionally (docs/FAMILIES_DESIGN.md §5.3) even before that family's
  // device list has loaded.
  const { devices: locatableDevices } = useLocatableDevices();

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
          {(isFamilyAdmin || locatableDevices.length > 0) && (
            <Button color="inherit" component={Link} href="/location" startIcon={<LocationOnIcon />}>
              Location
            </Button>
          )}
          {isFamilyAdmin && (
            <NavMenu
              label="Family"
              icon={<FamilyRestroomIcon />}
              links={familyLinks}
              badgeCount={openAlertCount}
            />
          )}
          <NavMenu label="Settings" icon={<SettingsIcon />} links={settingsLinks} />
          {isSuper && (
            <NavMenu label="Admin" icon={<AdminPanelSettingsIcon />} links={adminLinks} />
          )}
          <Box sx={{ flexGrow: 1 }} />
          {isSuper && <FamilySwitcher />}
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
