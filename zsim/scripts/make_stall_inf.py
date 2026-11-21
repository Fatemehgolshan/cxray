##!/usr/bin/env python3

import os
import re
import pandas as pd
from openpyxl import Workbook
from openpyxl.utils.dataframe import dataframe_to_rows
from openpyxl.styles import Font, Alignment
from openpyxl.utils import get_column_letter

# NEW: plotting
import matplotlib.pyplot as plt


def read_two_cdfs(stall_csv_path, access_csv_path):
    stall_df = pd.read_csv(stall_csv_path)   # percent_pages,percent_stall
    access_df = pd.read_csv(access_csv_path) # percent_pages,percent_access

    stall_df["percent_pages"] = stall_df["percent_pages"].astype(int)
    access_df["percent_pages"] = access_df["percent_pages"].astype(int)

    merged_df = pd.merge(
        stall_df,
        access_df,
        on="percent_pages",
        how="outer"
    ).sort_values("percent_pages")

    return merged_df


def autosize_columns(ws):
    for col_idx in range(1, ws.max_column + 1):
        col_letter = get_column_letter(col_idx)
        max_len = 0
        for cell in ws[col_letter]:
            if cell.value is None:
                continue
            max_len = max(max_len, len(str(cell.value)))
        ws.column_dimensions[col_letter].width = min(max_len + 2, 40)


def main():
    # change these if needed
    base_dir = os.getcwd()  # run this script from ~/repos/memtiering/zsim
    workloads = ["bc", "pr", "sssp", "bfs", "cc", "tc"]
    out_xlsx = os.path.join(base_dir, "stall_inf.xlsx")

    # NEW: output dir for plots
    plot_dir = os.path.join(base_dir, "stall_inf_plots")
    os.makedirs(plot_dir, exist_ok=True)

    # collect all page_* dirs across workloads
    page_dir_set = set()
    for wl in workloads:
        wl_dir = os.path.join(base_dir, wl)
        if not os.path.isdir(wl_dir):
            continue
        for name in os.listdir(wl_dir):
            if os.path.isdir(os.path.join(wl_dir, name)) and re.match(r"^page_.*", name):
                page_dir_set.add(name)

    page_dirs = sorted(page_dir_set)  # e.g., ["page_1G", "page_2M", "page_4K"]

    wb = Workbook()
    # remove default sheet
    wb.remove(wb.active)

    for page_dir in page_dirs:
        sheet_name = page_dir[:31]  # Excel limit
        ws = wb.create_sheet(title=sheet_name)

        # build a combined table for this page_dir
        combined_df = None

        for wl in workloads:
            stall_csv = os.path.join(base_dir, wl, page_dir, "rob_page_stall_cdf.csv")
            access_csv = os.path.join(base_dir, wl, page_dir, "page_access_cdf.csv")

            if not (os.path.isfile(stall_csv) and os.path.isfile(access_csv)):
                continue

            merged_df = read_two_cdfs(stall_csv, access_csv)

            # rename columns to include workload name
            merged_df = merged_df.rename(columns={
                "percent_stall": f"{wl}_stall",
                "percent_access": f"{wl}_access",
            })

            if combined_df is None:
                combined_df = merged_df
            else:
                combined_df = pd.merge(combined_df, merged_df, on="percent_pages", how="outer")

        if combined_df is None:
            # no data for this sheet
            ws["A1"] = f"No data found for {page_dir}"
            continue

        combined_df = combined_df.sort_values("percent_pages")

        # write dataframe into sheet
        for r_idx, row in enumerate(dataframe_to_rows(combined_df, index=False, header=True), start=1):
            ws.append(row)

        # style header
        header_font = Font(bold=True)
        for cell in ws[1]:
            cell.font = header_font
            cell.alignment = Alignment(horizontal="center")

        # freeze header row + first column
        ws.freeze_panes = "B2"

        # format numbers (2 decimals) for all non-percent_pages columns
        for col_idx in range(2, ws.max_column + 1):
            for row_idx in range(2, ws.max_row + 1):
                cell = ws.cell(row=row_idx, column=col_idx)
                if cell.value is not None:
                    cell.number_format = "0.00"

        autosize_columns(ws)

        # ============================================================
        # NEW: PLOTTING PART
        # We create two plots per page_dir:
        #   1) all *_stall columns vs percent_pages
        #   2) all *_access columns vs percent_pages
        # ============================================================

        # x-axis: percent_pages (10..100)
        x = combined_df["percent_pages"].astype(int)

        # ---------------------------
        # Plot 1: Stall curves
        # ---------------------------
        plt.figure()  # start a new figure
        plotted_any = False

        for wl in workloads:
            col = f"{wl}_stall"
            if col not in combined_df.columns:
                continue

            # y-axis data for this workload
            y = pd.to_numeric(combined_df[col], errors="coerce")
            if y.isna().all():
                continue

            plt.plot(x, y, marker="o", label=col)
            plotted_any = True

        if plotted_any:
            plt.xlabel("percent_pages")
            plt.ylabel("percent_stall")
            plt.title(f"{page_dir} - Stall CDF")
            plt.grid(True)
            plt.legend()
            plt.ylim(bottom=0)

            out_png = os.path.join(plot_dir, f"{page_dir}_stall.png")
            plt.savefig(out_png, dpi=200, bbox_inches="tight")
            print(f"Plotted: {out_png}")

        plt.close()  # close the figure (important inside loops)

        # ---------------------------
        # Plot 2: Access curves
        # ---------------------------
        plt.figure()
        plotted_any = False

        for wl in workloads:
            col = f"{wl}_access"
            if col not in combined_df.columns:
                continue

            y = pd.to_numeric(combined_df[col], errors="coerce")
            if y.isna().all():
                continue

            plt.plot(x, y, marker="o", label=col)
            plotted_any = True

        if plotted_any:
            plt.xlabel("percent_pages")
            plt.ylabel("percent_access")
            plt.title(f"{page_dir} - Access CDF")
            plt.grid(True)
            plt.legend()
            plt.ylim(bottom=0)

            out_png = os.path.join(plot_dir, f"{page_dir}_access.png")
            plt.savefig(out_png, dpi=200, bbox_inches="tight")
            print(f"Plotted: {out_png}")

        plt.close()

    wb.save(out_xlsx)
    print(f"Created: {out_xlsx}")


if __name__ == "__main__":
    main()

