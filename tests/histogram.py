import csv
import matplotlib.pyplot as plt

def load_latencies(csv_file):
    latencies = []
    with open(csv_file, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            avg = float(row["AvgLatency_us"])
            maxlat = float(row["MaxLatency_us"])
            # treat each test as a batch of identical latencies
            latencies.append(avg)
            latencies.append(maxlat)
    return latencies

def plot_histogram(latencies, title):
    plt.figure(figsize=(10, 6))
    plt.hist(latencies, bins=40)
    plt.title(title)
    plt.xlabel("Latency (µs)")
    plt.ylabel("Frequency")
    plt.grid(True)
    plt.show()

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print("Usage: python3 histogram.py benchmark.csv")
        exit(1)

    csv_path = sys.argv[1]
    latencies = load_latencies(csv_path)
    plot_histogram(latencies, "ZINF Raw Write Latency Distribution")

