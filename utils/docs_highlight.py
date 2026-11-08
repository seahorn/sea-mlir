# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Highlight MLIR and distinguish commands from output in documentation fences."""

from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexer import RegexLexer
from pygments.lexers import BashSessionLexer
from pygments.token import Comment, Keyword, Name, Number, Punctuation, String, Text


class MLIRLexer(RegexLexer):
    """Lex MLIR's SSA names, dialect operations, types and attributes."""

    tokens = {
        "root": [
            (r"//[^\n]*", Comment.Single),
            (r'"(?:\\.|[^"\\])*"', String),
            (r"%[\w.$-]+(?:#[0-9]+)?", Name.Variable),
            (r"@[\w.$-]+|\^[\w.$-]+", Name.Function),
            (
                r"![\w.]+|\b(?:i[0-9]+|f[0-9]+|index|tensor|vector|memref)\b",
                Keyword.Type,
            ),
            (r"#[\w.]+", Name.Attribute),
            (r"\b[a-zA-Z_][\w]*\.[\w.]+", Keyword),
            (r"\b(?:loc|align|inplace|to|true|false)\b", Keyword),
            (r"-?(?:0x[\da-fA-F]+|\d+(?:\.\d+)?)", Number),
            (r"[\[\]{}()<>,:=+*\-]", Punctuation),
            (r"\s+|[\w.$]+|.", Text),
        ]
    }


def format_fence(source, language, class_name, options, md, **kwargs):
    """Use Material's token palette, with a class for terminal styling."""
    lexer = MLIRLexer() if language == "mlir" else BashSessionLexer()
    return highlight(
        source,
        lexer,
        HtmlFormatter(cssclass=f"highlight language-{language}", wrapcode=True),
    )


def on_config(config):
    """Install fence formatters without a separately installed lexer package."""
    fences = config["mdx_configs"].setdefault("pymdownx.superfences", {})
    custom = fences.setdefault("custom_fences", [])
    for language in ("mlir", "console"):
        custom.append({"name": language, "class": language, "format": format_fence})
