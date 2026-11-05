# Configuration file for the Sphinx documentation builder (Godot-style Read the Docs setup)

project = "Noxiouse Engine"
copyright = "2026, Noxiouse Contributors"
author = "Noxiouse Contributors"
release = "2026.10"
version = "latest (2026.10)"

extensions = [
    "myst_parser",
    "sphinx_rtd_theme",
    "sphinx_copybutton",
    "sphinxcontrib.mermaid",
]

myst_enable_extensions = [
    "colon_fence",
    "deflist",
    "fieldlist",
    "tasklist",
    "attrs_inline",
]

myst_heading_anchors = 3

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

master_doc = "index"
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "index.html", "manual.html", "assets"]

html_theme = "sphinx_rtd_theme"
html_title = "Noxiouse Engine Documentation"
html_short_title = "Noxiouse Docs"

html_theme_options = {
    "logo_only": False,
    "prev_next_buttons_location": "both",
    "style_external_links": True,
    "vcs_pageview_mode": "",
    "style_nav_header_background": "#2980b9",
    "collapse_navigation": False,
    "sticky_navigation": True,
    "navigation_depth": 4,
    "includehidden": True,
    "titles_only": False,
}

html_static_path = ["assets"]
html_css_files = ["rtd-custom.css"]
