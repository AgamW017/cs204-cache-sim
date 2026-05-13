from __future__ import annotations

from io import StringIO
from pathlib import Path
import json
import re
from typing import Any, Iterable

import pandas as pd
import plotly.express as px
import streamlit as st


REPO_ROOT = Path(__file__).resolve().parents[1]
SAMPLE_TRACE_DIR = REPO_ROOT / "traces"

TRACE_COLUMNS = ["address", "reuse_distance", "stride"]
POLICY_COLUMNS = ["policy", "hits", "misses", "hit_rate", "source"]
POLICY_PATTERNS = {
    "hits": re.compile(r"Hits\s*:\s*(\d+)", re.IGNORECASE),
    "misses": re.compile(r"Misses\s*:\s*(\d+)", re.IGNORECASE),
    "hit_rate": re.compile(r"HitRate\s*:\s*([0-9.]+)", re.IGNORECASE),
    "policy": re.compile(r"^(LRU|FIFO|RANDOM|BELADY|CUSTOM)\b", re.IGNORECASE),
}


st.set_page_config(
    page_title="Cache Simulator Dashboard",
    page_icon="🧠",
    layout="wide",
    initial_sidebar_state="expanded",
)


st.markdown(
    """
    <style>
    .block-container {
        padding-top: 1.25rem;
        padding-bottom: 2rem;
    }
    .hero {
        padding: 1.2rem 1.4rem;
        border-radius: 1rem;
        background: linear-gradient(135deg, rgba(15, 23, 42, 0.96), rgba(30, 41, 59, 0.92));
        color: white;
        margin-bottom: 1rem;
        border: 1px solid rgba(148, 163, 184, 0.22);
        box-shadow: 0 12px 30px rgba(15, 23, 42, 0.18);
    }
    .hero h1 {
        margin: 0;
        font-size: 2rem;
        line-height: 1.1;
    }
    .hero p {
        margin: 0.35rem 0 0;
        color: rgba(226, 232, 240, 0.9);
        font-size: 0.95rem;
    }
    .metric-card {
        padding: 0.9rem 1rem;
        border-radius: 0.9rem;
        border: 1px solid rgba(96, 165, 250, 0.35);
        background: linear-gradient(180deg, rgba(219, 234, 254, 0.98), rgba(191, 219, 254, 0.92));
        box-shadow: 0 8px 20px rgba(37, 99, 235, 0.08);
        color: #0f172a;
    }
    .small-note {
        color: #1d4ed8;
        font-size: 0.85rem;
    }
    </style>
    """,
    unsafe_allow_html=True,
)


def _read_text(uploaded_file: Any) -> str:
    if uploaded_file is None:
        return ""
    data = uploaded_file.getvalue()
    return data.decode("utf-8", errors="replace")


def _candidate_name(file: Any) -> str:
    if file is None:
        return "unknown"
    return getattr(file, "name", "unknown")


@st.cache_data(show_spinner=False)
def parse_trace_text(text: str, source: str) -> pd.DataFrame:
    frame = pd.read_csv(StringIO(text), header=None, names=TRACE_COLUMNS)
    if frame.empty:
        return frame

    frame["source"] = source
    frame["address"] = frame["address"].astype(str)
    frame["address_int"] = frame["address"].apply(lambda value: int(value, 16))
    frame["reuse_distance"] = pd.to_numeric(frame["reuse_distance"], errors="coerce")
    frame["stride"] = pd.to_numeric(frame["stride"], errors="coerce")
    frame["abs_stride"] = frame["stride"].abs()
    frame["first_touch"] = frame["reuse_distance"] < 0
    frame["reused"] = ~frame["first_touch"]
    frame["stride_kind"] = pd.cut(
        frame["abs_stride"],
        bins=[-0.1, 0, 8, 64, 512, float("inf")],
        labels=["zero", "tiny", "small", "medium", "large"],
    )
    return frame


@st.cache_data(show_spinner=False)
def parse_policy_text(text: str, source: str) -> dict[str, Any]:
    stripped = text.strip()
    if not stripped:
        return {"policy": source, "source": source}

    # JSON result schema for future simulators.
    if stripped.startswith("{") or stripped.startswith("["):
        try:
            payload = json.loads(stripped)
            if isinstance(payload, list) and payload:
                payload = payload[0]
            if isinstance(payload, dict):
                row = {
                    "policy": payload.get("policy") or payload.get("name") or source,
                    "hits": payload.get("hits"),
                    "misses": payload.get("misses"),
                    "hit_rate": payload.get("hit_rate") or payload.get("hitrate"),
                    "source": source,
                }
                return _finalize_policy_row(row)
        except json.JSONDecodeError:
            pass

    # CSV result schema for future simulators.
    try:
        csv_frame = pd.read_csv(StringIO(stripped))
        if not csv_frame.empty and {"hits", "misses"}.issubset({c.lower() for c in csv_frame.columns}):
            row = {"policy": source, "source": source}
            lower_map = {c.lower(): c for c in csv_frame.columns}
            for key in ["policy", "hits", "misses", "hit_rate"]:
                if key in lower_map:
                    row[key] = csv_frame.iloc[0][lower_map[key]]
            return _finalize_policy_row(row)
    except Exception:
        pass

    # Current Week 2 text output.
    row: dict[str, Any] = {"policy": source, "source": source}
    for line in stripped.splitlines():
        policy_match = POLICY_PATTERNS["policy"].match(line.strip())
        if policy_match:
            row["policy"] = policy_match.group(1).upper()
        hits_match = POLICY_PATTERNS["hits"].search(line)
        if hits_match:
            row["hits"] = int(hits_match.group(1))
        misses_match = POLICY_PATTERNS["misses"].search(line)
        if misses_match:
            row["misses"] = int(misses_match.group(1))
        rate_match = POLICY_PATTERNS["hit_rate"].search(line)
        if rate_match:
            row["hit_rate"] = float(rate_match.group(1))
    return _finalize_policy_row(row)


def _finalize_policy_row(row: dict[str, Any]) -> dict[str, Any]:
    hits = pd.to_numeric(pd.Series([row.get("hits")]), errors="coerce").iloc[0]
    misses = pd.to_numeric(pd.Series([row.get("misses")]), errors="coerce").iloc[0]
    hit_rate = pd.to_numeric(pd.Series([row.get("hit_rate")]), errors="coerce").iloc[0]

    if pd.notna(hits):
        row["hits"] = int(hits)
    if pd.notna(misses):
        row["misses"] = int(misses)
    if pd.isna(hit_rate) and pd.notna(hits) and pd.notna(misses):
        total = float(hits + misses)
        hit_rate = 100.0 * float(hits) / total if total else 0.0
    if pd.notna(hit_rate):
        row["hit_rate"] = float(hit_rate)
    return row


def trace_summary_stats(frame: pd.DataFrame) -> dict[str, Any]:
    if frame.empty:
        return {}

    total = len(frame)
    reused = int(frame["reused"].sum())
    first_touches = int(frame["first_touch"].sum())
    reuse_rate = 100.0 * reused / total if total else 0.0
    avg_reuse_distance = frame.loc[frame["reused"], "reuse_distance"].mean()
    avg_abs_stride = frame["abs_stride"].mean()
    sequential = frame["abs_stride"].le(8).mean() * 100.0
    long_jump = frame["abs_stride"].gt(512).mean() * 100.0

    return {
        "total_accesses": total,
        "first_touches": first_touches,
        "reuse_rate": reuse_rate,
        "avg_reuse_distance": avg_reuse_distance,
        "avg_abs_stride": avg_abs_stride,
        "sequential_rate": sequential,
        "long_jump_rate": long_jump,
    }


def trace_summary_cards(stats: dict[str, Any]) -> list[tuple[str, str]]:
    def fmt(value: Any, suffix: str = "") -> str:
        if value is None or pd.isna(value):
            return "n/a"
        if isinstance(value, float):
            return f"{value:.2f}{suffix}"
        return f"{value}{suffix}"

    return [
        ("Total accesses", fmt(stats.get("total_accesses"))),
        ("First touches", fmt(stats.get("first_touches"))),
        ("Reuse rate", fmt(stats.get("reuse_rate"), "%")),
        ("Avg reuse distance", fmt(stats.get("avg_reuse_distance"))),
        ("Avg |stride|", fmt(stats.get("avg_abs_stride"))),
        ("Sequential / near access", fmt(stats.get("sequential_rate"), "%")),
        ("Long jumps", fmt(stats.get("long_jump_rate"), "%")),
    ]


def policy_table_from_inputs(uploaded_files: Iterable[Any]) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    for uploaded in uploaded_files:
        text = _read_text(uploaded)
        if not text.strip():
            continue
        rows.append(parse_policy_text(text, _candidate_name(uploaded)))
    if not rows:
        rows = [
            {"policy": "LRU", "hits": None, "misses": None, "hit_rate": None, "source": "manual"},
            {"policy": "FIFO", "hits": None, "misses": None, "hit_rate": None, "source": "manual"},
            {"policy": "RANDOM", "hits": None, "misses": None, "hit_rate": None, "source": "manual"},
            {"policy": "BELADY", "hits": None, "misses": None, "hit_rate": None, "source": "manual"},
        ]
    frame = pd.DataFrame(rows)
    for column in POLICY_COLUMNS:
        if column not in frame.columns:
            frame[column] = None
    return frame[POLICY_COLUMNS]


def make_trace_figures(frame: pd.DataFrame) -> dict[str, Any]:
    reused = frame.loc[frame["reused"]].copy()
    first_touches = int(frame["first_touch"].sum())
    if reused.empty:
        reuse_hist = px.line(title="Reuse distance (no reused accesses yet)")
    else:
        reuse_freq = reused["reuse_distance"].value_counts().sort_index().reset_index()
        reuse_freq.columns = ["reuse_distance", "count"]
        # omit low-frequency distances entirely
        reuse_freq = reuse_freq.loc[reuse_freq["count"] >= 10]
        if reuse_freq.empty:
            reuse_hist = px.line(title="Reuse distance (no distances with frequency >= 10)")
        else:
            reuse_hist = px.line(
                reuse_freq,
                x="reuse_distance",
                y="count",
                title="Reuse distance distribution (counts >= 10)",
                labels={"reuse_distance": "Reuse distance", "count": "Frequency"},
                markers=True,
            )
            reuse_hist.update_traces(line=dict(width=1.25), marker=dict(size=3))
            reuse_hist.update_layout(yaxis_type="log")

    # stride sign breakdown removed (no sign graph requested)

    stride_freq = (
        frame.loc[frame["stride"].notna()]
        .groupby("stride")
        .size()
        .reset_index(name="count")
        .sort_values(["count", "stride"], ascending=[False, True])
        .head(15)
    )
    top_stride_fig = px.bar(
        stride_freq,
        x="stride",
        y="count",
        title="Top 15 repeating strides",
        color="count",
        color_continuous_scale="Viridis",
    )
    top_stride_fig.update_traces(marker=dict(line=dict(width=1.5)))

    return {
        "reuse_hist": reuse_hist,
        "top_stride_fig": top_stride_fig,
    }


def policy_figures(frame: pd.DataFrame) -> dict[str, Any]:
    active = frame.copy()
    active["policy"] = active["policy"].fillna(active["source"])
    numeric = active.dropna(subset=["hit_rate", "hits", "misses"], how="all")

    if numeric.empty:
        return {}

    rate_fig = px.bar(
        numeric,
        x="policy",
        y="hit_rate",
        color="policy",
        title="Hit rate by policy",
        labels={"hit_rate": "Hit rate (%)"},
    )
    miss_fig = px.bar(
        numeric,
        x="policy",
        y="misses",
        color="policy",
        title="Miss count by policy",
        labels={"misses": "Misses"},
    )
    stacked = numeric.melt(
        id_vars=["policy"],
        value_vars=["hits", "misses"],
        var_name="metric",
        value_name="count",
    ).dropna(subset=["count"])
    stacked_fig = px.bar(
        stacked,
        x="policy",
        y="count",
        color="metric",
        barmode="stack",
        title="Hits vs misses",
        color_discrete_sequence=["#10b981", "#ef4444"],
    )

    return {"rate": rate_fig, "miss": miss_fig, "stacked": stacked_fig}


def render_trace_section(uploaded_trace: Any) -> None:
    st.subheader("Trace analysis")
    if uploaded_trace is None:
        sample_trace = SAMPLE_TRACE_DIR / "week1-trace.csv"
        if not sample_trace.exists():
            st.warning("Upload a Week 1 trace CSV file to view reuse-distance and stride analysis.")
            return
        st.info("No trace files uploaded. Using the sample traces from the repository.")
        uploaded_trace = sample_trace

    if isinstance(uploaded_trace, Path):
        text = uploaded_trace.read_text(encoding="utf-8", errors="replace")
        source = uploaded_trace.name
    else:
        text = _read_text(uploaded_trace)
        source = _candidate_name(uploaded_trace)

    frame = parse_trace_text(text, source)
    if frame.empty:
        st.warning("The selected trace file was empty or could not be parsed.")
        return

    stats = trace_summary_stats(frame)

    st.caption(f"Loaded trace: `{source}`")
    metrics = trace_summary_cards(stats)
    metric_columns = st.columns(len(metrics))
    for column, (label, value) in zip(metric_columns, metrics):
        with column:
            st.markdown(
                f"<div class='metric-card'><div class='small-note'>{label}</div><div style='font-size:1.35rem;font-weight:700'>{value}</div></div>",
                unsafe_allow_html=True,
            )

    figures = make_trace_figures(frame)
    st.plotly_chart(figures["reuse_hist"], use_container_width=True)

    st.plotly_chart(figures["top_stride_fig"], use_container_width=True)

    st.markdown("### Trace preview")
    st.dataframe(
        frame[["address", "reuse_distance", "stride", "abs_stride", "first_touch"]].head(50),
        use_container_width=True,
        hide_index=True,
    )


def render_policy_section(uploaded_policy_files: list[Any]) -> None:
    st.subheader("Replacement policy analysis")
    if uploaded_policy_files:
        st.caption("Parsed from uploaded result logs or structured outputs.")
    else:
        st.caption("Upload one or more result logs for LRU, FIFO, Random, Belady, or custom policies.")

    policy_frame = policy_table_from_inputs(uploaded_policy_files)
    editable = st.data_editor(
        policy_frame,
        use_container_width=True,
        hide_index=True,
        num_rows="dynamic",
        column_config={
            "policy": st.column_config.TextColumn("Policy"),
            "hits": st.column_config.NumberColumn("Hits", min_value=0, step=1),
            "misses": st.column_config.NumberColumn("Misses", min_value=0, step=1),
            "hit_rate": st.column_config.NumberColumn("Hit rate (%)", min_value=0.0, max_value=100.0, step=0.1),
            "source": st.column_config.TextColumn("Source"),
        },
        key="policy_table",
    )

    policy_figs = policy_figures(editable)
    if not policy_figs:
        st.info("Add hits, misses, and hit rate values to generate the comparison charts.")
        return

    left, right = st.columns(2)
    with left:
        st.plotly_chart(policy_figs["rate"], use_container_width=True)
    with right:
        st.plotly_chart(policy_figs["miss"], use_container_width=True)

    st.plotly_chart(policy_figs["stacked"], use_container_width=True)

    st.download_button(
        "Download comparison as CSV",
        editable.to_csv(index=False).encode("utf-8"),
        file_name="cache_policy_comparison.csv",
        mime="text/csv",
    )


def render_future_section() -> None:
    st.subheader("Future-ready data model")
    st.write(
        "This dashboard is designed to accept richer artifacts later without changing the UI flow. "
        "Current support is based on Week 1 trace CSVs and Week 2 textual hit/miss summaries."
    )

    left, right = st.columns(2)
    with left:
        st.markdown(
            """
            **Trace schema**

            ```csv
            address,reuse_distance,stride
            7ffe50153a88,-1,0
            7ffe50153a80,-1,-8
            ```
            """
        )
    with right:
        st.markdown(
            """
            **Policy schema**

            ```json
            {
              "policy": "LRU",
              "hits": 1200,
              "misses": 430,
              "hit_rate": 73.7
            }
            ```
            """
        )

    st.markdown(
        "- Belady, custom policies, and future simulators can expose JSON or CSV with the same fields.\n"
        "- The UI will automatically compare any uploaded policy artifacts with the same columns.\n"
        "- Trace files can be extended with more columns later; the dashboard will keep the core columns visible."
    )


def main() -> None:
    st.markdown(
        """
        <div class="hero">
            <h1>Cache Simulator Dashboard</h1>
            <p>Analyze memory traces, compare cache policies, and keep the layout ready for Belady and custom policies.</p>
        </div>
        """,
        unsafe_allow_html=True,
    )

    with st.sidebar:
        st.header("Inputs")
        trace_files = st.file_uploader(
            "Week 1 trace CSV file",
            type=["out", "csv", "txt"],
            accept_multiple_files=False,
            help="Upload a trace file with address,reuse_distance,stride columns.",
        )
        policy_files = st.file_uploader(
            "Policy result logs",
            type=["txt", "out", "log", "json", "csv"],
            accept_multiple_files=True,
            help="Upload one or more result files for LRU, FIFO, Random, Belady, or custom policies.",
        )
        st.divider()
        st.caption("Upload trace and policy artifacts, then switch tabs to inspect each analysis separately.")

    trace_tab, policy_tab, future_tab = st.tabs(["Trace analysis", "Replacement policy analysis", "Future-ready schema"])

    with trace_tab:
        render_trace_section(trace_files)

    with policy_tab:
        render_policy_section(policy_files or [])

    with future_tab:
        render_future_section()


if __name__ == "__main__":
    main()