"""Converts the Seung lab larval zebrafish hindbrain connectome
(Vishwanathan et al., Nature Neuroscience 2024) from its MATLAB file into the
plain-text cell and synapse list the fish brain loads.

Usage: python3 convert_connectome.py ZConnectome_04292021.mat hindbrain.txt
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matlab_file  # noqa: E402

# The authors' cell-type labels, and the names the fish brain uses.
TYPES = {
    "_Int_": "integrator",          # modO: the oculomotor velocity-to-position integrator
    "_Axl_": "axial",               # modA: the axial (body) module
    "_DOs_": "vestibular",          # descending octaval (vestibular) neurons
    "ABD_m": "abducens_motor",      # abducens motoneurons: turn the same-side eye outward
    "ABD_i": "abducens_internuclear",  # drive the other eye's medial rectus inward
    "vSPNs": "reticulospinal",      # spinal-projecting neurons, among them the Mauthner cell
}
MAUTHNER = 76202  # segment ID of the Mauthner cell, from the authors' analysis code


def main():
    source, target = sys.argv[1], sys.argv[2]
    connectome = matlab_file.load(source)["ZConnectome"]
    ids = [int(v) for v in connectome["cellID"].values]
    labels = connectome["cellType"].values
    labels = [label if isinstance(label, str) else "".join(label) for label in labels]
    matrix = connectome["connectome"]
    count = len(ids)
    if matrix.dims != [count, count] or any(label not in TYPES for label in labels) or MAUTHNER not in ids:
        sys.exit("convert_connectome: unexpected contents in " + source)

    synapses = []
    for post in range(count):
        for pre in range(count):
            n = int(matrix[post, pre])  # rows are postsynaptic
            if n:
                synapses.append((pre, post, n))
    with open(target, "w") as out:
        out.write("# Larval zebrafish hindbrain connectome, one side: cells and synapse counts.\n")
        out.write("# Vishwanathan, Sood, Wu, et al., Nature Neuroscience 27, 2443 (2024),\n")
        out.write("# doi:10.1038/s41593-024-01784-3; data from github.com/ashwinvish/Zfish_recon.\n")
        out.write("# Generated at build time; not redistributed with this project.\n")
        out.write(f"cells {count}\n")
        for index, (segment, label) in enumerate(zip(ids, labels)):
            kind = "mauthner" if segment == MAUTHNER else TYPES[label]
            out.write(f"{index} {segment} {kind}\n")
        out.write(f"synapses {len(synapses)}\n")
        for pre, post, n in synapses:
            out.write(f"{pre} {post} {n}\n")
    print(f"Hindbrain connectome: {count} cells, {len(synapses)} connections, {sum(s[2] for s in synapses)} synapses")


if __name__ == "__main__":
    main()
