from pathlib import Path
import json

from SCons.Script import Import

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
src_html = project_dir / "src" / "lib" / "wifi_config_portal.html"
dst_header = project_dir / "src" / "lib" / "wifi_config_portal_page.generated.h"


def generate_header(*args, **kwargs):
    html = src_html.read_text(encoding="utf-8")
    content = (
        "#ifndef SRC_LIB_WIFI_CONFIG_PORTAL_PAGE_GENERATED_H\n"
        "#define SRC_LIB_WIFI_CONFIG_PORTAL_PAGE_GENERATED_H\n\n"
        "static const char kWifiConfigPortalHtmlTemplate[] = "
        + json.dumps(html)
        + ";\n\n"
        "#endif\n"
    )
    dst_header.write_text(content, encoding="utf-8")
    print(f"[wifi_portal] generated {dst_header.name} from {src_html.name}")


generate_header()
