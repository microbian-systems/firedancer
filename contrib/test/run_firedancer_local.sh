#!/bin/bash

set -euxo pipefail
IFS=$'\n\t'

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

# create temporary files in the user's home directory because it's likely to be on a large disk
TMPDIR=$(mktemp --directory --tmpdir="$HOME" tmp-test-firedancer-fddev.XXXXXX)
cd $TMPDIR

cleanup() {
  sudo killall -9 -q fddev || true
  fddev configure fini all >/dev/null 2>&1 || true
  rm -rf "$TMPDIR"
}

trap cleanup EXIT SIGINT SIGTERM

FD_DIR="$SCRIPT_DIR/../.."

sudo killall -9 -q fddev || true

# if fd_frank_ledger is not on path then use the one in the home directory
if ! command -v fddev > /dev/null; then
  PATH="$FD_DIR/build/native/$CC/bin":$PATH
fi

#fddev configure fini all >/dev/null 2>&1

echo "Creating mint and stake authority keys..."
solana-keygen new --no-bip39-passphrase -o faucet.json > /dev/null
solana-keygen new --no-bip39-passphrase -o authority.json > /dev/null

# Create bootstrap validator keys
echo "Creating keys for Validator"
solana-keygen new --no-bip39-passphrase -o id.json > id.seed
solana-keygen new --no-bip39-passphrase -o vote.json > vote.seed
solana-keygen new --no-bip39-passphrase -o stake.json > stake.seed

# Start the bootstrap validator
while [ $(solana -u localhost epoch-info --output json | jq .blockHeight) -le 150 ]; do\
  sleep 1
done

_PRIMARY_INTERFACE=$(ip route show default | awk '/default/ {print $5}')
PRIMARY_IP=$(ip addr show $_PRIMARY_INTERFACE | awk '/inet / {print $2}' | cut -d/ -f1 | head -n1)

FULL_SNAPSHOT=$(wget -c -nc -S --trust-server-names http://$PRIMARY_IP:8899/snapshot.tar.bz2 |& grep 'location:' | cut -d/ -f2)

echo "
[layout]
    affinity = \"1-32\"
    bank_tile_count = 1
[gossip]
    port = 8700
[tiles]
    [tiles.gossip]
        entrypoints = [\"$PRIMARY_IP\"]
        peer_ports = [8001]
        gossip_listen_port = 8700
    [tiles.repair]
        repair_intake_listen_port = 8701
        repair_serve_listen_port = 8702
    [tiles.replay]
        snapshot = \"$FULL_SNAPSHOT\"
        tpool_thread_count = 10
        funk_sz_gb = 8
        funk_rec_max = 100000
        funk_txn_max = 1024
[log]
    path = \"fddev.log\"
    level_stderr = \"NOTICE\"
[development]
    topology = \"firedancer\"
" > fddev.toml

sudo gdb --args $FD_DIR/build/native/$CC/bin/fddev --log-path $(readlink -f fddev.log) --config $(readlink -f fddev.toml) --no-solana --no-sandbox --no-clone

grep -q "bank_hash" $(readlink -f fddev.log)
if grep -q "Bank hash mismatch" $(readlink -f fddev.log); then
  echo "*** BANK HASH MISMATCH ***"
fi
