"""PCB builder on top of KiCad's pcbnew Python API (run with KiCad's bundled
python). Builds a board from the schematic project model (sch.Project):
footprints, nets, placement, outline, zones and keep-outs; then optionally
routes it with Freerouting through a Specctra DSN/SES round trip.
"""

import math
import os
import subprocess
import sys

import pcbnew

KICAD_FP = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
FREEROUTING = ("/private/tmp/claude-501/-Users-albert-src-pager/df4dc366-ba40-4c7e-83de-0bfb3a07db72/"
               "scratchpad/freerouting/freerouting-2.5.0.jar")


def mm(v):
    return pcbnew.FromMM(v)


def V(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


def apply_design_settings(board, thickness=1.0):
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(mm(thickness))
    ds.m_TrackMinWidth = mm(0.1)
    ds.m_ViasMinSize = mm(0.45)
    ds.m_MinThroughDrill = mm(0.2)
    ds.m_MinClearance = mm(0.127)
    ds.m_CopperEdgeClearance = mm(0.3)
    ds.m_SolderMaskExpansion = mm(0.05)
    try:
        nc = ds.m_NetSettings.GetDefaultNetclass()
        nc.SetTrackWidth(mm(0.2))
        nc.SetClearance(mm(0.15))
        nc.SetViaDiameter(mm(0.5))
        nc.SetViaDrill(mm(0.25))
    except Exception as e:  # noqa: BLE001
        print("netclass setup skipped:", e)


class Board:
    def __init__(self, prj, project_dir, name, thickness=1.0):
        self.prj = prj
        self.dir = project_dir
        self.name = name
        self.board = pcbnew.BOARD()
        self.board.SetFileName(os.path.join(project_dir, name + ".kicad_pcb"))
        apply_design_settings(self.board, thickness)
        self.nets = {}
        self.fps = {}
        self.pending = {}  # ref -> (inst, sheet)
        self._make_nets()

    # ---- nets -------------------------------------------------------------
    def _make_nets(self):
        for name in sorted(self.prj.nets):
            ni = pcbnew.NETINFO_ITEM(self.board, name)
            self.board.Add(ni)
            self.nets[name] = ni

    # ---- footprints -------------------------------------------------------
    def _lib_dir(self, lib):
        if lib == "pager-carrier":
            return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pager-carrier.pretty"))
        return os.path.join(KICAD_FP, lib + ".pretty")

    def load_all(self):
        """Create one footprint per schematic component (all units merged)."""
        for ref, (inst, sheet) in self.prj.components.items():
            if ref.startswith("#"):
                continue
            fpid = inst.fields.get("Footprint", "")
            if not fpid or ":" not in fpid:
                print("no footprint for", ref)
                continue
            lib, name = fpid.split(":", 1)
            fp = pcbnew.FootprintLoad(self._lib_dir(lib), name)
            if fp is None:
                raise RuntimeError("footprint not found: %s (%s)" % (fpid, ref))
            fp.SetReference(ref)
            fp.SetValue(inst.fields.get("Value", ""))
            fp.SetFPID(pcbnew.LIB_ID(lib, name))
            fp.SetPath(pcbnew.KIID_PATH("/%s/%s/%s" % (self.prj.root_uuid, sheet.symbol_uuid, inst.uuid)))
            user_fields = [k for k in inst.fields if k not in ("Reference", "Value", "Footprint")]
            for k in user_fields:
                try:
                    fp.SetField(k, inst.fields.get(k) or "")
                except Exception as e:  # noqa: BLE001
                    print("field", k, e)
            for f in fp.GetFields():
                if f.GetName() in user_fields and f.GetName() not in ("Datasheet", "Description"):
                    f.SetVisible(False)
                    f.SetLayer(pcbnew.F_Fab)
            attr = fp.GetAttributes()
            if inst.dnp:
                attr |= pcbnew.FP_DNP
            if inst.in_bom:
                attr &= ~pcbnew.FP_EXCLUDE_FROM_BOM
            else:
                attr |= pcbnew.FP_EXCLUDE_FROM_BOM
            fp.SetAttributes(attr)
            # nets: collect from every unit of this reference
            pinnets = {}
            for r2, (i2, s2) in self.prj.components.items():
                pass
            for sh in self.prj.sheets:
                for i2 in sh.instances:
                    if i2.ref == ref:
                        pinnets.update(i2.nets)
            for pad in fp.Pads():
                num = str(pad.GetNumber())
                n = pinnets.get(num)
                if n and n in self.nets:
                    pad.SetNet(self.nets[n])
                elif num in pinnets and n is None:
                    # no-connect pin: KiCad's parity check expects this net name
                    try:
                        pname = inst.sym.pin(num).name
                    except KeyError:
                        pname = num
                    ncname = "unconnected-(%s-%s-Pad%s)" % (ref, pname.replace("/", "{slash}"), num)
                    if ncname not in self.nets:
                        ni = pcbnew.NETINFO_ITEM(self.board, ncname)
                        self.board.Add(ni)
                        self.nets[ncname] = ni
                    pad.SetNet(self.nets[ncname])
            self.board.Add(fp)
            self.fps[ref] = fp
        return self.fps

    def place(self, ref, x, y, rot=0, bottom=False):
        fp = self.fps[ref]
        fp.SetPosition(V(x, y))
        fp.SetOrientationDegrees(rot)
        if bottom:
            fp.Flip(V(x, y), False)
        return fp

    def fp_size(self, ref):
        fp = self.fps[ref]
        fp.BuildCourtyardCaches()
        bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
        if bb.GetWidth() == 0:
            bb = fp.GetBoundingBox(False, False)
        return pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())

    def pack(self, refs, x0, y0, x1, y1, rot=0, gap=0.3):
        """Shelf-pack footprints into a rectangle, left to right, top to bottom."""
        x, y, rowh = x0, y0, 0.0
        for ref in refs:
            fp = self.fps[ref]
            fp.SetOrientationDegrees(rot)
            w, h = self.fp_size(ref)
            if x + w > x1 + 1e-6 and x > x0:
                x = x0
                y += rowh + gap
                rowh = 0.0
            fp.SetPosition(V(x + w / 2, y + h / 2))
            x += w + gap
            rowh = max(rowh, h)
        if y + rowh > y1 + 0.01:
            print("WARNING: pack overflow at", refs[0], "by", round(y + rowh - y1, 2), "mm")
        return y + rowh

    # ---- drawing ----------------------------------------------------------
    def seg(self, a, b, layer=pcbnew.Edge_Cuts, w=0.1):
        s = pcbnew.PCB_SHAPE(self.board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(V(*a))
        s.SetEnd(V(*b))
        s.SetLayer(layer)
        s.SetWidth(mm(w))
        self.board.Add(s)
        return s

    def arc(self, start, mid, end, layer=pcbnew.Edge_Cuts, w=0.1):
        s = pcbnew.PCB_SHAPE(self.board)
        s.SetShape(pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(V(*start), V(*mid), V(*end))
        s.SetLayer(layer)
        s.SetWidth(mm(w))
        self.board.Add(s)
        return s

    def outline_polyline(self, pts, radius=0.0):
        """Closed outline through pts (list of (x, y)); optional corner fillets
        for 90-degree corners when radius > 0 (axis-aligned segments only)."""
        n = len(pts)
        if radius <= 0:
            for i in range(n):
                self.seg(pts[i], pts[(i + 1) % n])
            return
        # build fillet points
        segs = []
        for i in range(n):
            p0, p1, p2 = pts[i - 1], pts[i], pts[(i + 1) % n]
            d1 = (p1[0] - p0[0], p1[1] - p0[1])
            d2 = (p2[0] - p1[0], p2[1] - p1[1])
            l1 = math.hypot(*d1)
            l2 = math.hypot(*d2)
            u1 = (d1[0] / l1, d1[1] / l1)
            u2 = (d2[0] / l2, d2[1] / l2)
            r = min(radius, l1 / 2 - 0.01, l2 / 2 - 0.01)
            a = (p1[0] - u1[0] * r, p1[1] - u1[1] * r)
            b = (p1[0] + u2[0] * r, p1[1] + u2[1] * r)
            # arc midpoint: corner point pulled toward the centre
            cx = p1[0] - u1[0] * r + u2[0] * r
            cy = p1[1] - u1[1] * r + u2[1] * r
            m = (cx + (p1[0] - cx) * (1 - 1 / math.sqrt(2)), cy + (p1[1] - cy) * (1 - 1 / math.sqrt(2)))
            segs.append((a, m, b))
        for i in range(n):
            a, m, b = segs[i]
            self.arc(a, m, b)
            nxt = segs[(i + 1) % n][0]
            self.seg(b, nxt)

    def text(self, s, x, y, layer=pcbnew.F_SilkS, size=1.0, rot=0, thickness=0.15):
        t = pcbnew.PCB_TEXT(self.board)
        t.SetText(s)
        t.SetPosition(V(x, y))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(thickness))
        t.SetTextAngleDegrees(rot)
        if layer in (pcbnew.B_SilkS, pcbnew.B_Fab, pcbnew.B_Cu):
            t.SetMirrored(True)
        self.board.Add(t)
        return t

    def edge_keepout(self, pts, width=0.45):
        """Track/via keep-out band just inside a closed outline (list of points),
        so the autorouter honours the copper-to-edge clearance."""
        n = len(pts)
        for i in range(n):
            a, b = pts[i], pts[(i + 1) % n]
            dx, dy = b[0] - a[0], b[1] - a[1]
            L = (dx * dx + dy * dy) ** 0.5
            if L == 0:
                continue
            nx, ny = -dy / L, dx / L  # inward normal for a clockwise outline (y down)
            quad = [(a[0], a[1]), (b[0], b[1]), (b[0] + nx * width, b[1] + ny * width), (a[0] + nx * width, a[1] + ny * width)]
            self.keepout(quad, tracks=True, vias=True, pour=False, fps=False, name="edge")

    def zone(self, net, pts, layer, name=""):
        z = pcbnew.ZONE(self.board)
        z.SetLayer(layer)
        if net:
            z.SetNet(self.nets[net])
        z.SetZoneName(name)
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetMinThickness(mm(0.2))
        z.SetLocalClearance(mm(0.2))
        z.SetThermalReliefGap(mm(0.25))
        z.SetThermalReliefSpokeWidth(mm(0.3))
        try:
            z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        except AttributeError:
            pass
        ol = z.Outline()
        ol.NewOutline()
        for x, y in pts:
            ol.Append(mm(x), mm(y))
        self.board.Add(z)
        return z

    def keepout(self, pts, layers=("F.Cu", "B.Cu"), tracks=True, vias=True, pour=True, fps=True, name=""):
        z = pcbnew.ZONE(self.board)
        z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(tracks)
        z.SetDoNotAllowVias(vias)
        z.SetDoNotAllowZoneFills(pour)
        z.SetDoNotAllowPads(False)
        try:
            z.SetDoNotAllowFootprints(fps)
        except AttributeError:
            pass
        ls = pcbnew.LSET()
        for l in layers:
            ls.addLayer(self.board.GetLayerID(l))
        z.SetLayerSet(ls)
        z.SetZoneName(name)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in pts:
            ol.Append(mm(x), mm(y))
        self.board.Add(z)
        return z

    @staticmethod
    def circle_pts(cx, cy, r, n=24):
        return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]

    # ---- finish ----------------------------------------------------------
    def fill(self):
        filler = pcbnew.ZONE_FILLER(self.board)
        filler.Fill(self.board.Zones())

    def save(self, path=None):
        path = path or os.path.join(self.dir, self.name + ".kicad_pcb")
        pcbnew.SaveBoard(path, self.board)
        return path

    def route(self, passes=12):
        dsn = os.path.join(self.dir, "out", self.name + ".dsn")
        ses = os.path.join(self.dir, "out", self.name + ".ses")
        os.makedirs(os.path.dirname(dsn), exist_ok=True)
        if not pcbnew.ExportSpecctraDSN(self.board, dsn):
            raise RuntimeError("DSN export failed")
        cmd = ["java", "-jar", FREEROUTING, "-de", dsn, "-do", ses, "-mp", str(passes), "-mt", "8"]
        print("+", " ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1500)
        tail = (r.stdout + r.stderr)[-1500:]
        print(tail)
        if not os.path.exists(ses):
            raise RuntimeError("no .ses produced")
        if not pcbnew.ImportSpecctraSES(self.board, ses):
            raise RuntimeError("SES import failed")
        return ses
