import csv
import matplotlib.pyplot as plt

def load_results(csvfile):
    chunks = []
    speeds = []
    with open(csvfile) as f:
        r = csv.DictReader(f)
        for row in r:
            chunks.append(int(row["SectorsWritten"]))
            speeds.append(float(row["Throughput_KBps"]))
    return chunks, speeds

def plot(chunks, speeds):
    plt.figure(figsize=(12, 6))
    plt.plot(chunks, speeds, marker='o')
    plt.xlabel("Chunks (sectors written per batch)")
    plt.ylabel("Throughput (KB/s)")
    plt.title("ZINF Raw Throughput vs Chunk Size")
    plt.grid(True)
    plt.show()

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print("Usage: python3 throughput_graph.py benchmark.csv")
        exit(1)
    chunks, speeds = load_results(sys.argv[1])
    plot(chunks, speeds)

