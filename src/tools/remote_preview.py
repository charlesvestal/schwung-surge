#!/usr/bin/env python3
"""Bundle the Surge XT Remote UI into ONE HTML file with a mock bridge.

The shipped page is web_ui.html plus assets/, served by Schwung Manager beside
its bridge script, with the plugin's remote/*.json files next to it. To look at
it with no device, inline everything (fonts as data URIs) and replay a capture
of those files from tests/remote/fixtures/ through src/remote/dev/mock-bridge.js.

    python3 src/tools/remote_preview.py [out.html]
"""
import base64, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src/remote")
FIX = os.path.join(ROOT, "tests/remote/fixtures")
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-preview/surge-remote-preview.html")

def read(rel): return open(os.path.join(SRC, rel), encoding="utf-8").read()
def fixture(name):
    p = os.path.join(FIX, name + ".json")
    return json.load(open(p)) if os.path.exists(p) else None

html = read("web_ui.html")
css = read("assets/style.css")
# fonts inline
def font_uri(m):
    fn = m.group(1)
    data = base64.b64encode(open(os.path.join(SRC, "assets", fn), "rb").read()).decode()
    return 'url("data:font/woff2;base64,%s")' % data
css = re.sub(r'url\("(fonts/[^"]+)"\)', font_uri, css)

fx = {k: fixture(k) for k in ("state", "chain_params", "patch_params", "presets_index", "waves", "filters")}
fixture_js = "window.SURGE_FIXTURE = " + json.dumps(fx, separators=(",", ":")) + ";"

html = html.replace('<link rel="stylesheet" href="assets/style.css">', "<style>\n" + css + "\n</style>")
html = html.replace('<script src="/static/schwung-remote-api.js"></script>', "<script>\n" + fixture_js + "\n</script>\n<script>\n" + read("dev/mock-bridge.js") + "\n</script>")
for name in ("surge-meta.js", "surge-format.js", "core.js", "widgets.js", "views.js", "app.js"):
    html = html.replace('<script src="assets/%s"></script>' % name, "<script>\n" + read("assets/" + name) + "\n</script>")
html = html.replace("<title>Surge XT</title>", "<title>Surge XT Remote</title>")
body = re.search(r"<head>(.*)</head>\s*<body>(.*)</body>", html, re.S)
page = body.group(1).replace('<meta charset="utf-8">', "").replace('<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">', "") + body.group(2)

os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "w").write(page.strip() + "\n")
have = [k for k, v in fx.items() if v]
print("wrote %s (%d KB); fixtures: %s" % (os.path.relpath(out, ROOT), len(page) // 1024, ", ".join(have) or "none"))
