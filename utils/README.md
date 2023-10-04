pkt-set-cfg-parser.py is the script that generates RSS configurations and packet dispatcher.
```
usage: pkt-set-cfg-parser.py [-h] [--nf-path NF_PATH] config_file

Parse packet set configuration file

positional arguments:
  config_file        path to the configuration file

optional arguments:
  -h, --help         show this help message and exit
  --nf-path NF_PATH  path to the nf
```
Note: Currently in the example NFs, the RSS configurations and packet dispatcher are already generated, so
you do not need to run this script before building these NFs.
These RSS configs and packet dispactcher are manually written due to legacy and are equivalent to the ones auto-generated from this script.