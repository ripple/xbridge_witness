# Testing

The attestation server is a work in progress. There are planned non-backward
compatible design changes and the current server has known bugs and security
issues (for example, a non-privileged request can send a "stop" command).
However, it can be used to start writing tests against. These tests can help
shake out the bugs in the proposed transactors, and these tests can be updated
as the design changes.


## Configuration parameters

There are a couple configuration files that are needed. 


## xbridge_witness config file
The attestation server uses a json config file. Here's an example:

```json
{
  "LockingChainEndpoint": {
    "IP": "127.0.0.1",
    "Port": 6005
  },
  "IssuingChainEndpoint": {
    "IP": "127.0.0.2",
    "Port": 6007
  },
  "RPCEndpoint": {
    "IP": "127.0.0.3",
    "Port": 6010
  },
  "DBDir": "/home/swd/data/witness/witness0/db",
  "SigningKeySeed": "snwitEjg9Mr8n65cnqhATKcd1dQmv",
  "SigningKeyType": "ed25519",
  "LockingChainRewardAccount": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
  "IssuingChainRewardAccount": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
  "XChainBridge": {
    "LockingChainDoor": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
    "LockingChainIssue": "XRP",
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": "XRP"
  }
}
```

It contains the websocket endpoints for the sidechain and mainchain, the port
that clients will use to connect to this server ("rpc_endpoint"), the directory
to store the sql database ("db_dir"), the secret key used to sign attestations
("signing_key_seed") and the sidechain spec.

The server knows the location of this file by using the `--conf` command line
argument.

# rippled config 

The rippled servers must be from the `attest` branch of my personal github
(https://github.com/seelabs/rippled/tree/attest). The configuration file can be
set-up as normal, but must enable the `Sidechains` amendment in the `features`
stanza.

# python scripts bootstrap file

The python scripts need a configuration file that used to setup the door
account. It needs the account id and signing keys. An example of such a config
is:

```json
{
  "mainchain_door": {
    "id": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
    "secret_key": "<xxxx>"
  },
  "sidechain_door": {
    "id": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "secret_key": "masterpassphrase"
  }
}
```

## Setting up the sidechain 

The `XChainCreateBridge` transaction is used to attach a sidechain to a door
account on both the mainchain and the sidechain. The account must be one of the
door accounts listed in the sidechain spec, and the two door accounts must be
unique.

An example of such a transaction is:

```json
{
  "Account": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
  "TransactionType": "XChainCreateBridge",
  "Sidechain": {
    "src_chain_door": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
    "src_chain_issue": "XRP",
    "dst_chain_door": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "dst_chain_issue": "XRP"
  },
  "SignerQuorum": 1,
  "SignerEntries": [
    {
      "SignerEntry": {
        "Account": "rwq63hfLK1b1WjuFpxWC8vS1gMGtZuqZfN",
        "SignerWeight": 1
      }
    }
  ]
}
```

The `SignerEntries` indirectly contains the public keys that can be used to sign
attestations: it's an account derived from a public key, not a public key
itself. A future update will change this so it contains the public key itself.
Also note this will usually contain multiple keys. The transaction above was
used for a simple test.

The `Sidechain` spec contains the door accounts and what the cross chain assets
are. Note that the assets are always exchanged 1:1, and the assets must both be
either IOUs or both be XRP. IOUs are specified the same way as amounts except
they don't have a "value" field.

## A end-to-end cross chain transactions

The steps to send a cross chain transaction are:

1) Use a `SidechainXChainSeqNumCreate` transaction to reserve a sequence number
on the sidechain. Right now, there is not good RPC command to get the sequence
number that was checked out. But for now, know they start at 1 and increment up.
An RPC command will be added (you should also be able to get the number from the
transaction's metadata in a validated ledger). This sequence number must be
checked out on the chain meant to receive the funds from the cross chain
transaction.

An example of such a transaction is:
```json
{
  "Account": "rG5r16gHYktYHrLyiWzMMbKQAFRptZe5rH",
  "TransactionType": "SidechainXChainSeqNumCreate",
  "Sidechain": {
    "src_chain_door": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
    "src_chain_issue": "XRP",
    "dst_chain_door": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "dst_chain_issue": "XRP"
  }
}
```

2) Use a `SidechainXChainTransfer` transaction to start a transfer on the chain
sending funds. This transaction must reference the `XChainSequence` number that
was checked out in step (1). The account that controls this sequence number
controls the funds, so it's important to get this right.

An example of such a transaction is:

```json
{
  "Account": "rp5xs35BvW9etAiVJcjjsVb2sYE3iWdBAB",
  "TransactionType": "SidechainXChainTransfer",
  "Sidechain": {
    "src_chain_door": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
    "src_chain_issue": "XRP",
    "dst_chain_door": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "dst_chain_issue": "XRP"
  },
  "Amount": "100000000",
  "XChainSequence": 1
}
```

3) Use the `xbridge_witness` servers to collect the attestations. To collect the
signatures from one server, send an `http` `POST` request to the server. The
request must contain parameters that specify the amount, sequence number,
dst_door (door account the funds were sent to) and the sidechain spec. The
command name is `witness`. An example of such a request is:

```json
{
  "method": "witness",
  "params": [
    {
      "amount": "100000000",
      "xchain_sequence_number": "1",
      "dst_door": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
      "sidechain": {
        "src_chain_door": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
        "src_chain_issue": "XRP",
        "dst_chain_door": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
        "dst_chain_issue": "XRP"
      }
    }
  ]
}
```

Using curl to get the proof from the server looks like (assuming the request is
in the file `witness_request_data.json`):
```json
curl --header "Content-Type: application/json" \
  --request POST \
  --data "@witness_request_data.json" \
  --silent \
http://127.0.0.3:6010
```

The actual proof is in the the field `result.proof`. Using `jq` to extract this
proof, pipe the above command to:

```bash
 | jq '.result.proof'
```

If the server does not agree that the funds were send to the door account, an
error is returning saying "no such transaction".

4) Using the proof from above, send a `SidechainXChainClaim` transaction from
the account that owns the cross chain sequence number. An example of such a transaction is:

```json
{
  "Account": "rG5r16gHYktYHrLyiWzMMbKQAFRptZe5rH",
  "TransactionType": "SidechainXChainClaim",
  "XChainClaimProof": {
    "sidechain": {
      "src_chain_door": "rhWQzvdmhf5vFS35vtKUSUwNZHGT53qQsg",
      "src_chain_issue": "XRP",
      "dst_chain_door": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
      "dst_chain_issue": "XRP"
    },
    "amount": "100000000",
    "signatures": [
      {
        "signature": "E390B09BEE808CFCCE9E9893198016D938619AF922C3CD081747F47712C996054B0D735DF6F11E1E98A2DED40075328A610C659C39E7E02B38CD9FDB9134A90A",
        "signing_key": "aKGZxX59uhMhXgc2UTR2evekjHDiUy4LysrU8zMjk2vyRdFPLYi9"
      }
    ],
    "was_src_chain_send": true,
    "xchain_seq": 1
  },
  "Destination": "rrdE8g5xLsfbRkANrWPbMo7VtfkfiGhGc"
}
```

If everything works, the funds will be moved from the door account to the
specified destination and the cross chain sequence number will be destroyed.
