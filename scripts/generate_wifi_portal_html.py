from pathlib import Path
import json
import re

include_pattern = re.compile(r"<!--\s*@include\s+([A-Za-z0-9_./-]+)\s*-->")
template_pattern = re.compile(
    r"<!--\s*@template\s+([A-Za-z0-9_]+)\s*-->(.*?)<!--\s*@endtemplate\s*-->",
    re.S,
)

# (source html, generated header, include guard macro, C++ template variable)
PAGES = [
    (
        "wifi_config_portal.html",
        "wifi_config_portal_page.generated.h",
        "SRC_LIB_WIFI_CONFIG_PORTAL_PAGE_GENERATED_H",
        "kWifiConfigPortalHtmlTemplate",
    ),
    (
        "wifi_update_portal.html",
        "wifi_update_portal_page.generated.h",
        "SRC_LIB_WIFI_UPDATE_PORTAL_PAGE_GENERATED_H",
        "kWifiUpdatePortalHtmlTemplate",
    ),
]

TEMPLATE_FILES = [
    (
        "wifi_config_portal_sections.html",
        "wifi_config_portal_sections.generated.h",
        "SRC_LIB_WIFI_CONFIG_PORTAL_SECTIONS_GENERATED_H",
    ),
]


def resolve_project_dir():
    # Under PlatformIO the build env exposes $PROJECT_DIR; when run standalone
    # fall back to the repository root (this script lives in <root>/scripts/).
    try:
        from SCons.Script import Import

        Import("env")
        return Path(env.subst("$PROJECT_DIR"))  # noqa: F821
    except Exception:
        return Path(__file__).resolve().parent.parent


def read_html_with_includes(path, seen=None):
    if seen is None:
        seen = set()
    path = path.resolve()
    if path in seen:
        raise RuntimeError(f"recursive include in {path}")
    seen.add(path)
    html = path.read_text(encoding="utf-8")

    def replace_include(match):
        include_path = (path.parent / match.group(1)).resolve()
        if not include_path.is_file():
            raise FileNotFoundError(f"missing html include: {include_path}")
        return read_html_with_includes(include_path, seen)

    html = include_pattern.sub(replace_include, html)
    seen.remove(path)
    return html


def generate_headers(*args, **kwargs):
    lib_dir = resolve_project_dir() / "src" / "lib"
    for src_name, dst_name, guard, var_name in PAGES:
        html = read_html_with_includes(lib_dir / src_name)
        content = (
            f"#ifndef {guard}\n"
            f"#define {guard}\n\n"
            f"static const char {var_name}[] = "
            + json.dumps(html)
            + ";\n\n"
            "#endif\n"
        )
        (lib_dir / dst_name).write_text(content, encoding="utf-8")
        print(f"[wifi_portal] generated {dst_name} from {src_name} and includes")

    for src_name, dst_name, guard in TEMPLATE_FILES:
        html = read_html_with_includes(lib_dir / src_name)
        templates = template_pattern.findall(html)
        if not templates:
            raise RuntimeError(f"no @template blocks found in {src_name}")
        lines = [f"#ifndef {guard}", f"#define {guard}", ""]
        for name, body in templates:
            value = body.strip()
            lines.append(f"static const char kWifiConfigPortal{name.title().replace('_', '')}Template[] = " + json.dumps(value) + ";")
        lines.append("")
        lines.append("#endif")
        lines.append("")
        (lib_dir / dst_name).write_text("\n".join(lines), encoding="utf-8")
        print(f"[wifi_portal] generated {dst_name} from {src_name} templates")


generate_headers()
