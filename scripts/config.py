#! /usr/bin/env python
import json
import os
import subprocess
import sys

from optparse import OptionParser
from sets import Set

#JSON keys
DOCKER_NETWORK="docker-network"
NETWORK="network"
AGENTIP="agent-ip"
VXLAN="vxlan"
VXLANIP="vxlan-ip"
VTEPMAC="vtep-mac"
AGENTS="agents"
VTEPS="vteps"
MACS="macs"

required_keys = set([
  DOCKER_NETWORK,
  NETWORK,
  AGENTIP,
  VXLAN,
  VXLANIP,
  VTEPMAC,
  AGENTS,
  VTEPS,
  MACS
])

PBR_TABLE_NO=42
PBR_TABLE_NAME="mesos-pbr"

def exec_call(cmd, errMsg):
  try:
    subprocess.check_call(cmd)
  except subprocess.CalledProcessError as err:
    sys.exit(errMsg + ":" + str(err))

def exec_output(cmd, errMsg):
  try:
    output  = subprocess.check_output(cmd)
    return output
  except subprocess.CalledProcessError as err:
    sys.exit(errMsg + ":" + str(err))


def main():
  usage = "usage: %prog [options] arg1 arg2"
  parser = OptionParser()

  (options, args) = parser.parse_args();

  if len(args) < 1:
    print "Require overlay config to specified."
    sys.exit(1)

  try:
    fp = open(args[0], 'r')
    overlayInfo = json.load(fp)
  except Exception as err:
    print "Unable to load the overlay config:", err
    sys.exit(1)

  for key in required_keys:
    if not key in overlayInfo: 
      print "Unable to find key %s in overlay config" % (key)
      sys.exit(1)
    else:
      print "Found key ", key

  networkUUID = exec_output(
      ["docker",
      "network",
      "create",
      "--driver=bridge",
      "--subnet=" + overlayInfo[NETWORK],
      overlayInfo[DOCKER_NETWORK]],
      "Unable to create the docker network")
  networkUUID=networkUUID.lstrip().rstrip()

  print "Created docker network %s with UUID:%s" % \
      (overlayInfo[DOCKER_NETWORK], networkUUID)

  networkInfo = exec_output(
      ["docker",
      "network",
      "inspect",
      networkUUID],
      "Unable to inspect the docker network")

  networkInfo = json.loads(networkInfo)
  print "JSON:", networkInfo

  # Add the PBR rule.
  exec_call(
      ["ip",
      "rule",
      "add",
      "from",
      overlayInfo[NETWORK],
      "lookup",
      str(PBR_TABLE_NO)],
      "Unable to create the PBR rule.")

  # Create the VTEP
  exec_call(
      ["ip",
      "link",
      "add",
      "vtep" + overlayInfo[VXLAN],
      "type",
      "vxlan",
      "id",
      overlayInfo[VXLAN],
      "local",
      overlayInfo[AGENTIP]],
      "Unable to create the VTEP" + overlayInfo[VXLAN])

  # Bring the VTEP up.
  exec_call(
      ["ip",
      "link",
      "set",
      "vtep" + overlayInfo[VXLAN],"up"],
      "Unable to bring VTEP" + overlayInfo[VXLAN])

  # Configure an ip-address on the VTEP.
  exec_call(
      ["ip",
      "addr",
      "add",
      overlayInfo[VXLANIP]+"/8",
      "dev",
      "vtep"+overlayInfo[VXLAN]], 
      "Unable to assign IP address to vtep" +\
      overlayInfo[VXLAN])

  # Set VTEP mac-address.
  exec_call(
  ["ip",
  "link",
  "set",
  "dev",
  "vtep" + overlayInfo[VXLAN],
  "address",
  overlayInfo[VTEPMAC]],
  "Unable to set address for VTEP" + overlayInfo[VXLAN])

  # Setup ARP cache for all known VTEP.
  for vtepIP in overlayInfo[MACS].keys():
    exec_call(
        ["arp",
        "-s",
        vtepIP,
        overlayInfo[MACS][vtepIP]],
        "Unable to add ARP cache for " + vtepIP)

  # Add routes into the PBR for Agent subnets.
  for agent in overlayInfo[AGENTS].keys():
    exec_call(
        ["ip",
        "route",
        "add",
        overlayInfo[AGENTS][agent],
        "via",
        overlayInfo[VTEPS][agent],
        "dev",
        "vtep" + overlayInfo[VXLAN],
        "table",
        str(PBR_TABLE_NO)],
        "Unable to add route to agent subnet " +
        overlayInfo[AGENTS][agent] + " in PBR")

  # Add routes into the PBR for each Agent.
  for agent in overlayInfo[VTEPS].keys():
    exec_call(
        ["ip",
        "route",
        "add",
        agent,
        "via",
        overlayInfo[VTEPS][agent],
        "dev",
        "vtep" + overlayInfo[VXLAN],
        "table",
        str(PBR_TABLE_NO)], 
        "Unable to add route to agent " +\
        overlayInfo[VTEPS][agent] + " in PBR")

  # Add entries into the VxLAN FDB for each VTEP endpoint.
  for agent in overlayInfo[VTEPS].keys(): 
    exec_call(
        ["bridge",
         "fdb",
         "add",
         overlayInfo[MACS][overlayInfo[VTEPS][agent]],
         "dev",
         "vtep" + overlayInfo[VXLAN],
         "dst",
         agent], 
        "Unable to add FDB entry for " + agent + " in VXLAN")


if __name__ == "__main__":
  main()

