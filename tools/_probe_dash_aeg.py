"""Count Asset parts whose name does NOT start with 'AEG' but contains '-AEG' (cross-tile
proxy), split by _00 vs non-_00. If _00 count is 0, generalizing aeg_row_from_name to accept
'-AEG' changes NOTHING for the _00 passes (safe)."""
import extract_all_items as E, config
from pathlib import Path
MSB = Path(config.require_err_mod_dir())/'map'/'MapStudio'
from collections import Counter
c={'_00':0,'lod':0}; models_00=Counter(); models_lod=Counter()
for mp in sorted(MSB.glob('*.msb.dcx')):
    stem=mp.name.replace('.msb.dcx',''); is00=stem.endswith('_00')
    try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
    except: continue
    for a in msb.Parts.Assets:
        nm=str(a.Name)
        if not nm.startswith('AEG') and 'AEG' in nm:
            k='_00' if is00 else 'lod'; c[k]+=1
            (models_00 if is00 else models_lod)[str(a.ModelName)]+=1
print("Asset parts NOT starting 'AEG' but containing AEG:")
print("  _00 tiles:", c['_00'], "  non-_00:", c['lod'])
print("  _00 models:", dict(models_00.most_common(10)))
print("  lod models (top):", dict(models_lod.most_common(12)))
