import json
import os
import matplotlib.pyplot as plt

def extract_fio_data(filepath):
    """Extracts write clat_ns mean and bandwidth from a fio JSON file."""
    if not os.path.exists(filepath):
        print(f"Warning: {filepath} not found.")
        return None, None

    with open(filepath, 'r') as f:
        data = json.load(f)

    try:
        job = data['jobs'][0]
        clat_ns = job['write']['clat_ns']['mean']
        bw_kbps = job['write']['bw']
        bw_mbps = bw_kbps / 1024.0
        return clat_ns, bw_mbps
    except KeyError as e:
        print(f"Error parsing {filepath}: Missing key {e}")
        return None, None

def main():
    raw_dir = "raw"
    fresh_path = os.path.join(raw_dir, "fresh.json")
    dirty_path = os.path.join(raw_dir, "dirty.json")

    # Extract metrics
    fresh_clat, fresh_bw = extract_fio_data(fresh_path)
    dirty_clat, dirty_bw = extract_fio_data(dirty_path)

    # Prepare data for plotting
    labels = []
    clat_data = []
    bw_data = []

    if fresh_clat is not None:
        labels.append("Fresh")
        clat_data.append(fresh_clat)
        bw_data.append(fresh_bw)

    if dirty_clat is not None:
        labels.append("Dirty (Stale Reuse)")
        clat_data.append(dirty_clat)
        bw_data.append(dirty_bw)

    if not labels:
        print("No valid data found to plot. Exiting.")
        return

    # Set up matplotlib style
    plt.style.use('ggplot')
    colors = ['#4C72B0', '#DD8452'] # Clean blue and orange

    # ---------------------------------------------------------
    # Chart 1: Latency (clat_ns)
    # ---------------------------------------------------------
    fig, ax = plt.subplots(figsize=(6, 5))
    bars = ax.bar(labels, clat_data, color=colors[:len(labels)], width=0.5)
    ax.set_ylabel("Mean Completion Latency (ns)", fontweight='bold')

    # Add values on top of bars
    for bar in bars:
        yval = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2, yval + (max(clat_data) * 0.02),
                f"{yval:,.0f} ns", ha='center', va='bottom', fontweight='bold')

    plt.tight_layout()
    plt.savefig('latency_chart.png', dpi=300)
    plt.close()
    print("-> Generated latency_chart.png")

    # ---------------------------------------------------------
    # Chart 2: Throughput (MB/s)
    # ---------------------------------------------------------
    fig, ax = plt.subplots(figsize=(6, 5))
    bars = ax.bar(labels, bw_data, color=colors[:len(labels)], width=0.5)
    ax.set_ylabel("Write Throughput (MB/s)", fontweight='bold')

    # Add values on top of bars
    for bar in bars:
        yval = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2, yval + (max(bw_data) * 0.02),
                f"{yval:,.0f} MB/s", ha='center', va='bottom', fontweight='bold')

    plt.tight_layout()
    plt.savefig('throughput_chart.png', dpi=300)
    plt.close()
    print("-> Generated throughput_chart.png")

if __name__ == "__main__":
    main()
