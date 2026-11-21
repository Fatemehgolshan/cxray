#!/usr/bin/env python3

import os
import re
import pandas as pd
from pandas.errors import EmptyDataError
from openpyxl import Workbook
from openpyxl.utils.dataframe import dataframe_to_rows
from openpyxl.styles import Font, Alignment
from openpyxl.utils import get_column_letter
import matplotlib.pyplot as plt


# ------------------------------------------------------------
# Read one stall_minus_access.csv safely
# ------------------------------------------------------------
def read_stall_minus_access(csv_path):
    """
    Reads:
        percent_pages,diff_percent

    Returns:
        df or None
    """
    try:
        df = pd.read_csv(csv_path)
    except EmptyDataError:
        return None
    except Exception:
        return None

    if df.empty:
        return None

    if "percent_pages" not in df.columns or "diff_percent" not in df.columns:
        return None

    # convert to numeric and drop bad rows
    df["percent_pages"] = pd.to_numeric(df["percent_pages"], errors="coerce")
    df["diff_percent"] = pd.to_numeric(df["diff_percent"], errors="coerce")
    df = df.dropna(subset=["percent_pages", "diff_percent"])

    if df.empty:
        return None

    df["percent_pages"] = df["percent_pages"].astype(int)
    return df.sort_values("percent_pages")


# ------------------------------------------------------------
# Auto-size Excel columns
# ------------------------------------------------------------
def autosize_columns(ws):
    for col_idx in range(1, ws.max_column + 1):
        col_letter = get_column_letter(col_idx)
        max_len = 0
        for cell in ws[col_letter]:
            if cell.value is None:
                continue
            max_len = max(max_len, len(str(cell.value)))
        ws.column_dimensions[col_letter].width = min(max_len + 2, 50)


# ------------------------------------------------------------
# Main
# ------------------------------------------------------------
def main():
    # Run from: ~/repos/memtiering/zsim
    base_dir = os.getcwd()
    workloads = ["bc", "pr", "sssp", "bfs", "cc", "tc"]

    # Output files
    out_xlsx = os.path.join(base_dir, "stall_minus_access.xlsx")
    plot_dir = os.path.join(base_dir, "stall_minus_access_plots")
    os.makedirs(plot_dir, exist_ok=True)

    # --------------------------------------------------------
    # Find all page_* directories
    # --------------------------------------------------------
    page_dir_set = set()
    for wl in workloads:
        wl_dir = os.path.join(base_dir, wl)
        if not os.path.isdir(wl_dir):
            continue

        for name in os.listdir(wl_dir):
            if re.match(r"^page_.*", name) and os.path.isdir(os.path.join(wl_dir, name)):
                page_dir_set.add(name)

    page_dirs = sorted(page_dir_set)

    # --------------------------------------------------------
    # Create Excel workbook
    # --------------------------------------------------------
    wb = Workbook()
    wb.remove(wb.active)

    # --------------------------------------------------------
    # Process each page_*
    # --------------------------------------------------------
    for page_dir in page_dirs:
        ws = wb.create_sheet(title=page_dir[:31])
        combined_df = None

        # ----------------------------------------------------
        # Collect data from all workloads
        # ----------------------------------------------------
        for wl in workloads:
            csv_path = os.path.join(base_dir, wl, page_dir, "stall_minus_access.csv")
            if not os.path.isfile(csv_path):
                continue

            df = read_stall_minus_access(csv_path)
            if df is None:
                continue

            # rename diff column to include workload name
            df = df.rename(columns={"diff_percent": f"{wl}_diff"})

            if combined_df is None:
                combined_df = df
            else:
                combined_df = pd.merge(combined_df, df, on="percent_pages", how="outer")

        if combined_df is None:
            ws["A1"] = f"No valid data for {page_dir}"
            continue

        combined_df = combined_df.sort_values("percent_pages")

        # ----------------------------------------------------
        # Write dataframe to Excel sheet
        # ----------------------------------------------------
        for row in dataframe_to_rows(combined_df, index=False, header=True):
            ws.append(row)

        # Header formatting
        for cell in ws[1]:
            cell.font = Font(bold=True)
            cell.alignment = Alignment(horizontal="center")

        ws.freeze_panes = "B2"

        # Number format (2 decimals)
        for col_idx in range(2, ws.max_column + 1):
            for row_idx in range(2, ws.max_row + 1):
                cell = ws.cell(row=row_idx, column=col_idx)
                if cell.value is not None:
                    cell.number_format = "0.00"

        autosize_columns(ws)

        # ====================================================
        # 🔹 PLOTTING PART (IMPORTANT)
        # ====================================================

        # x-axis values: percent of pages
        x = combined_df["percent_pages"].astype(int)

        # create a new figure (one chart per page size)
        plt.figure()

        # loop over workload columns
        for col in combined_df.columns:

            # skip x-axis column
            if col == "percent_pages":
                continue

            # convert column to numeric
            y = pd.to_numeric(combined_df[col], errors="coerce")

            # skip if column has no valid data
            if y.isna().all():
                continue

            # plot workload curve
            plt.plot(
                x,          # x-axis: percent_pages
                y,          # y-axis: stall_minus_access
                marker="o", # show points
                label=col   # legend label (e.g., cc_diff)
            )

        # axis labels
        plt.xlabel("percent_pages")
        plt.ylabel("stall_minus_access (%)")

        # title = page size (page_4K, page_2M, ...)
        plt.title(page_dir)

        # grid + legend
        plt.grid(True)
        plt.legend()

        # save figure
        out_png = os.path.join(plot_dir, f"{page_dir}.png")
        plt.savefig(out_png, dpi=200, bbox_inches="tight")

        # close figure (important inside loops)
        plt.close()

        print(f"Plotted: {out_png}")

    # --------------------------------------------------------
    # Save Excel file
    # --------------------------------------------------------
    wb.save(out_xlsx)
    print(f"Created: {out_xlsx}")


if __name__ == "__main__":
    main()

