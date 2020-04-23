import os, socket
import json

sd = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sd.connect(("login1.wrangler.tacc.utexas.edu", 5672))

rpcs = []
with open("wrangler_nids") as fd:
    for line in fd:
        try:
            nid, fqdn, hn = line.split()
        except: continue    
        rpcs += [{"hostname" : hn, "nid" : nid}]
sd.sendall(json.dumps(rpcs))
        