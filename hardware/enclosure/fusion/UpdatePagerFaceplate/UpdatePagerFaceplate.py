"""Fusion script: swap the printed cassette parts in the open pager assembly for the latest exports.

Run from Fusion: Utilities > Add-Ins > Scripts tab > UpdatePagerFaceplate > Run, with the pager
assembly open and active.  Steps:
  1. saves a new version of the open document first (checkpoint, nothing changed yet)
  2. deletes top-level occurrences whose component name starts with one of REPLACE
  3. imports each STEP in IMPORT_FILES into the root component; the STEPs are already in the
     assembly frame (same frame as the Assembled*.step exports), so no transform is applied;
     the new occurrences are grounded
  4. saves again with a description listing what was imported
Nothing else in the assembly is touched (the carrier, Walter, battery, tin, keyboard stay as placed).
"""
import adsk.core, adsk.fusion, traceback, os

OUT = '/Users/albert/src/pager/hardware/enclosure/out'
IMPORT_FILES = ['faceplate.step', 'slider_cap.step', 'fl_cap.step', 'panel_board_placed.step']
REPLACE = ('faceplate', 'slider_cap', 'fl_cap', 'panel_board_placed')


def run(context):
    app = adsk.core.Application.get()
    ui = app.userInterface
    try:
        doc = app.activeDocument
        design = adsk.fusion.Design.cast(app.activeProduct)
        if design is None:
            ui.messageBox('Open the pager assembly (a Fusion design) first.')
            return
        root = design.rootComponent
        missing = [f for f in IMPORT_FILES if not os.path.exists(os.path.join(OUT, f))]
        if missing:
            ui.messageBox('Missing STEP files in %s:\n%s' % (OUT, '\n'.join(missing)))
            return

        # 1. checkpoint version before touching anything
        if not doc.isSaved:
            ui.messageBox('Document has never been saved; save it once by hand first.')
            return
        saved = doc.save('Checkpoint before UpdatePagerFaceplate (nothing changed yet)')
        if not saved and doc.isModified:
            ui.messageBox('Could not save the checkpoint version; stopping before any change.')
            return

        # 2. remove the old printed parts (top level only)
        removed = []
        for occ in list(root.occurrences):
            nm = occ.component.name.lower()
            if nm.startswith(REPLACE):
                removed.append(occ.component.name)
                occ.deleteMe()

        # 3. import the new ones, already in the assembly frame
        im = app.importManager
        imported = []
        for f in IMPORT_FILES:
            path = os.path.join(OUT, f)
            opts = im.createSTEPImportOptions(path)
            opts.isViewFit = False
            before = root.occurrences.count
            if not im.importToTarget(opts, root):
                ui.messageBox('Import failed for %s; the checkpoint version is intact.' % f)
                return
            for i in range(before, root.occurrences.count):
                occ = root.occurrences.item(i)
                try:
                    occ.isGrounded = True
                except Exception:
                    pass
            imported.append(f)

        # 4. save the result as a new version
        doc.save('UpdatePagerFaceplate: imported %s; removed %s' % (', '.join(imported), ', '.join(removed) or 'nothing'))
        ui.messageBox('Done.\nRemoved: %s\nImported: %s\nTwo new versions saved (checkpoint + update).'
                      % (', '.join(removed) or 'nothing', ', '.join(imported)))
    except Exception:
        if ui:
            ui.messageBox('UpdatePagerFaceplate failed:\n{}'.format(traceback.format_exc()))
