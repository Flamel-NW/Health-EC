import subprocess,sys
p=subprocess.run([sys.argv[1],"--scenario","dynamic_degradation","--num-reads","99999","--format","csv"],capture_output=True,text=True)
if p.returncode==0 or "--num-reads divisible by 20" not in p.stderr:raise SystemExit("wrong negative-case failure")
