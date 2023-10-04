import json
import sys
import argparse

# Parse command line arguments
parser = argparse.ArgumentParser(description="Parse packet set configuration file")
parser.add_argument("config_file", help="path to the configuration file")
parser.add_argument("--nf-path", help="path to the nf", default=".")
args = parser.parse_args()

# Open the JSON file and load its contents into a Python object
with open(args.config_file, 'r') as f:
    data = json.load(f)
cfg = data["cfg"]
if type(cfg) != list:
    cfg = [cfg]

# Retrieve list of NICs that receive packet sets
nic_groups = []
for nic_group in cfg:
    nic_group = nic_group["nic"]
    if type(nic_group) != list:
        nic_group = [nic_group]
    nic_groups.append(nic_group)

## 1. Generate RSS config and check for compatibility
# TODO: Check against specific NIC
# TODO: Support the case where orphan packets are received on different NICs than the packet sets
# TODO: Merge the case of NFs without packet sets
RSS_MAP_L2 = {"eth": "ETH_RSS_ETH"}
RSS_MAP_L3 = {"ipv4": "ETH_RSS_IP",
              "ipv6": "ETH_RSS_IP"}
RSS_MAP_L4 = {"udp": "ETH_RSS_UDP",
              "tcp": "ETH_RSS_TCP"}
RSS_MAP_L3_SINGLE_FIELD = {"src_ip": "ETH_RSS_L3_SRC_ONLY",
                           "dst_ip": "ETH_RSS_L3_DST_ONLY"}
RSS_MAP_L4_SINGLE_FIELD = {"src_port": "ETH_RSS_L4_SRC_ONLY",
                           "dst_port": "ETH_RSS_L4_DST_ONLY"}

rss_hfs = [""]
num_imcompatible_layers = 0
imcompatibility_report = ""
rss_symmetric = "false"
cfg_nic_group_one = cfg[0]

if len(cfg) == 2:
    # Check and parse
    rss_hfs.append("")
    cfg_nic_group_two = cfg[1]
    for cfg1_layer, cfg2_layer in zip(cfg_nic_group_one["id"], cfg_nic_group_two["id"]):
        layer = cfg1_layer["layer"]
        proto = cfg1_layer["proto"]
        if type(cfg1_layer["hdr fields"]) != list:
            cfg1_layer["hdr fields"] = [cfg1_layer["hdr fields"]]
        if type(cfg2_layer["hdr fields"]) != list:
            cfg2_layer["hdr fields"] = [cfg2_layer["hdr fields"]]

        is_imcompatible = False
        if layer == 2:
            if "eth" in proto:
                if len(cfg1_layer["hdr fields"]) == 1 or len(cfg2_layer["hdr fields"]) == 1:
                    is_imcompatible = True
                    imcompatibility_report += "Layer 2: " + proto + " cannot specify src_mac or dst_mac only\n"
                else:
                    rss_hfs[0] += RSS_MAP_L2[proto] + " | "
                    rss_hfs[1] += RSS_MAP_L2[proto] + " | "
            else:
                is_imcompatible = True
                imcompatibility_report += "Layer 2: " + proto + " not supported\n"
        elif layer == 3:
            if "ip" in proto:
                rss_hfs[0] += RSS_MAP_L3[proto] + " | "
                rss_hfs[1] += RSS_MAP_L3[proto] + " | "
                if len(cfg1_layer["hdr fields"]) == 1 and len(cfg2_layer["hdr fields"]) == 1:
                    rss_hfs[0] += RSS_MAP_L3_SINGLE_FIELD[cfg1_layer["hdr fields"][0]] + " | "
                    rss_hfs[1] += RSS_MAP_L3_SINGLE_FIELD[cfg2_layer["hdr fields"][0]] + " | "
            else:
                is_imcompatible = True
                imcompatibility_report += "Layer 3: " + proto + " not supported\n"
        else:
            for substr in proto.split("|"):
                if "udp" == substr or "tcp" == substr:
                    rss_hfs[0] += RSS_MAP_L4[substr] + " | "
                    rss_hfs[1] += RSS_MAP_L4[substr] + " | "
                    if len(cfg1_layer["hdr fields"]) == 1 and len(cfg2_layer["hdr fields"]) == 1:
                        rss_hfs[0] += RSS_MAP_L4_SINGLE_FIELD[cfg1_layer["hdr fields"][0]] + " | "
                        rss_hfs[1] += RSS_MAP_L4_SINGLE_FIELD[cfg2_layer["hdr fields"][0]] + " | "
                else:
                    is_imcompatible = True
                    imcompatibility_report += "Layer 4: " + substr + " not supported\n"

        if is_imcompatible:
            num_imcompatible_layers += 1
        
        if len(cfg1_layer["hdr fields"]) == 2 and len(cfg2_layer["hdr fields"]) == 2:
            if cfg1_layer["hdr fields"][0] == cfg2_layer["hdr fields"][1] \
            and cfg1_layer["hdr fields"][1] == cfg2_layer["hdr fields"][0]:
                rss_symmetric = "true"
else:
    for cfg_layer in cfg_nic_group_one["id"]:
        layer = cfg_layer["layer"]
        proto = cfg_layer["proto"]
        if type(cfg_layer["hdr fields"]) != list:
            cfg_layer["hdr fields"] = [cfg_layer["hdr fields"]]

        is_imcompatible = False
        if layer == 2:
            if "eth" in proto:
                if len(cfg_layer["hdr fields"]) == 1:
                    is_imcompatible = True
                    imcompatibility_report += "Layer 2: " + proto + " cannot specify src_mac or dst_mac only\n"
                else:
                    rss_hfs[0] += RSS_MAP_L2[proto] + " | "
            else:
                is_imcompatible = True
                imcompatibility_report += "Layer 2: " + proto + " not supported\n"
        elif layer == 3:
            if "ip" in proto:
                rss_hfs[0] += RSS_MAP_L3[proto] + " | "
                if len(cfg_layer["hdr fields"]) == 1:
                    rss_hfs[0] += RSS_MAP_L3_SINGLE_FIELD[cfg_layer["hdr fields"][0]] + " | "
            else:
                is_imcompatible = True
                imcompatibility_report += "Layer 3: " + proto + " not supported\n"
        else:
            for substr in proto.split("|"):
                if "udp" == substr or "tcp" == substr:
                    rss_hfs[0] += RSS_MAP_L4[substr] + " | "
                    if len(cfg_layer["hdr fields"]) == 1:
                        rss_hfs[0] += RSS_MAP_L4_SINGLE_FIELD[cfg_layer["hdr fields"][0]] + " | "
                else:
                    is_imcompatible = True
                    imcompatibility_report += "Layer 4: " + substr + " not supported\n"

        if is_imcompatible:
            num_imcompatible_layers += 1
        
# Result
rss_gen_succ = True
num_layers = len(cfg_nic_group_one["id"])
if num_imcompatible_layers == num_layers:
    rss_gen_succ = False
    print("ERROR: Incompatible packet set configuration")
    print(imcompatibility_report)
    sys.exit(1)
else:
    if "num nics" in data:
        num_nics = data["num nics"]
    else:
        num_nics = 0
        for nic_group in nic_groups:
            num_nics += len(nic_group)
    # Default RSS for NICs that do not receive packet sets
    rss_hf_by_nic = ["ETH_RSS_IP | ETH_RSS_TCP | ETH_RSS_UDP"] * num_nics

    for nic_group, rss_hf in zip(nic_groups, rss_hfs):
        rss_hf = rss_hf[:-3]
        for nic in nic_group:
            rss_hf_by_nic[nic]= rss_hf


## 2. Generate packet set definitions
## 3. Generate packet parser and dispatcher
# TODO: Support ipv6 and eth
PKT_FIELD_TYPE = {"eth": {},
                  "ipv4": {"src_ip": "uint32_t", "dst_ip": "uint32_t", "proto": "uint8_t"},
                  "ipv6": {},
                  "tcp": {"src_port": "uint16_t", "dst_port": "uint16_t"},
                  "udp": {"src_port": "uint16_t", "dst_port": "uint16_t"}
}
PROTO_HEADER_MAP = {"eth": "ether_header",
                    "ipv4": "ipv4_header",
                    "ipv6": "",
                    "tcp": "tcpudp_header",
                    "udp": "tcpudp_header"}
PKT_FIELD_MAP = {"src_port": "src_port",
                 "dst_port": "dst_port",
                 "src_ip": "src_addr",
                 "dst_ip": "dst_addr"}

if rss_gen_succ:
    pkt_set_def_fields = ""
    pkt_set_id_eq_fields = ""
    pkt_set_id_hash_fields = ""

    for cfg_layer in cfg_nic_group_one["id"]:
        layer = cfg_layer["layer"]
        proto = cfg_layer["proto"]
        if proto == "tcp|udp" or proto == "udp|tcp":
            # Add field to differentiate the current layer
            field_type = "uint8_t"
            field_name = "layer" + str(layer) + "_proto" 
            pkt_set_def_fields += "  " + field_type + " " + field_name + ";\n"
            pkt_set_id_eq_fields += f"    (id1->{field_name} == id2->{field_name}) &&\n"
            # TODO: serialize the pkt set id into bitstream for hash
            pkt_set_id_hash_fields += f"  hash = __builtin_ia32_crc32si(hash, id->{field_name});\n"
            # No difference between tcp and udp here in terms of field length
            proto = "tcp"
        if type(cfg_layer["hdr fields"]) != list:
            cfg_layer["hdr fields"] = [cfg_layer["hdr fields"]]
        for i, field in enumerate(cfg_layer["hdr fields"]):
            field_type = PKT_FIELD_TYPE[proto][field]
            field_name = f"layer{layer}" + "_" + f"field{i}"
            pkt_set_def_fields += "  " + field_type + " " + field_name + ";\n"
            pkt_set_id_eq_fields += f"    (id1->{field_name} == id2->{field_name}) &&\n"
            # TODO: serialize the pkt set id into bitstream for hash
            pkt_set_id_hash_fields += f"  hash = __builtin_ia32_crc32si(hash, id->{field_name});\n"

    pkt_set_def = "struct pkt_set_id {\n"
    pkt_set_def += pkt_set_def_fields
    pkt_set_def += "};\n\n"
    pkt_set_def += "bool pkt_set_id_eq(void* a, void* b);\n\n" \
                 + "unsigned pkt_set_id_hash(void* obj);\n\n" \
                 + "void pkt_set_state_allocate(void *obj);\n\n" \
                 + "struct pkt_set_state {\n" \
                 + "  // To be filled in by the user\n" \
                 + "};\n"
                
    pkt_set_utils = "#include \"nf.h\"\n"   
    pkt_set_utils += "#include \"pkt_set.gen.h\"\n\n"
    pkt_set_utils += "bool pkt_set_id_eq(void* a, void* b) {\n" \
                   + "  pkt_set_id_t *id1 = (pkt_set_id_t *) a;\n" \
                   + "  pkt_set_id_t *id2 = (pkt_set_id_t *) b;\n" \
                   + "  return\n" \
                   + pkt_set_id_eq_fields[:-4] + ";\n}\n\n"
    pkt_set_utils += "unsigned pkt_set_id_hash(void* obj) {\n" \
                   + "  pkt_set_id_t *id = (pkt_set_id_t *) obj;\n\n" \
                   + "  unsigned hash = 0;\n" \
                   + pkt_set_id_hash_fields \
                   + "  return hash;\n}\n\n"
    pkt_set_utils += "void pkt_set_state_allocate(void *obj) {\n" \
                   + "// This function is invoked at NF initialization\n" \
                   + "// You can use it to initialize a packet set's state to constant values or leave it empty\n" \
                   + "}\n"

    # Generate packet parser
    pkt_set_utils += "\n// Packet parser\n" \
                   + "bool nf_pkt_parser(uint8_t *buffer, pkt_t *pkt){\n"
    # TODO: Support ipv6
    pkt_parser_layers = ""
    layers = []
    for cfg_layer in cfg_nic_group_one["id"]:
        layer = cfg_layer["layer"]
        if layer == 2:
            layers.append(2)
        elif layer == 3:
            layers += [2, 3]
        else:
            layers += [2, 3, 4]
    layers = list(set(layers))
    layers.sort()
    for layer in layers:
        if layer == 2:
            pkt_parser_layers += "  pkt->ether_header = nf_then_get_ether_header(buffer);\n\n"
        elif layer == 3:
            pkt_parser_layers += "  uint8_t *ip_options;\n" \
                               + "  pkt->ipv4_header = nf_then_get_ipv4_header(pkt->ether_header, buffer, &ip_options);\n" \
                               + "  if (pkt->ipv4_header == NULL) {\n" \
                               + "    NF_DEBUG(\"Malformed IPv4, dropping\");\n" \
                               + "    return false;\n" \
                               + "  }\n\n"
        else:
            pkt_parser_layers += "  pkt->tcpudp_header = nf_then_get_tcpudp_header(pkt->ipv4_header, buffer);\n" \
                               + "  if (pkt->tcpudp_header == NULL) {\n" \
                               + "    NF_DEBUG(\"Not TCP/UDP, dropping\");\n" \
                               + "    return false;\n" \
                               + "  }\n\n"
    pkt_set_utils += pkt_parser_layers \
                   + "  // payload is not used here\n" \
                   + "  pkt->payload = NULL;\n\n" \
                   + "  pkt->raw = buffer;\n\n" \
                   + "  return true;\n}\n"

    # Generate packet dispatcher
    # TODO: Support the case where orphan packets are received on different NICs than the packet sets
    # TODO: Merge the case of NFs without packet sets
    pkt_set_utils += "\n// Packet dispatcher\n" \
                   + "int nf_pkt_dispatcher(const pkt_t *pkt, uint16_t incoming_dev, pkt_set_id_t *pkt_set_id, bool *has_pkt_set_state, nf_state_t *non_pkt_set_state) {\n" \
                   + "  int pkt_class;\n" \
                   + "  switch(incoming_dev) {\n"
    for nic_group_cfg in cfg:
        pkt_set_id_fields = ""
        for cfg_layer in nic_group_cfg["id"]:
            layer = cfg_layer["layer"]
            proto = cfg_layer["proto"]
            if proto == "tcp|udp" or proto == "udp|tcp":
                # Add field to differentiate the current layer
                field_name = "layer" + str(layer) + "_proto" 
                # TODO: support ipv6
                pkt_set_id_fields += f"      pkt_set_id->{field_name} = pkt->ipv4_header->next_proto_id;\n"
                # No difference between tcp and udp here in terms of field length
                proto = "tcp"
            if type(cfg_layer["hdr fields"]) != list:
                cfg_layer["hdr fields"] = [cfg_layer["hdr fields"]]
            for i, field in enumerate(cfg_layer["hdr fields"]):
                field_name = f"layer{layer}" + "_" + f"field{i}"
                pkt_set_id_fields += f"      pkt_set_id->{field_name} = pkt->{PROTO_HEADER_MAP[proto]}->{PKT_FIELD_MAP[field]};\n" 
        nic_group = nic_group_cfg["nic"]
        if type(nic_group) != list:
            nic_group = [nic_group]
        for nic in nic_group:
            pkt_set_utils += f"    case {nic}:\n"
        pkt_set_utils += pkt_set_id_fields \
                       + "      *has_pkt_set_state = true;\n" \
                       + "      pkt_class = 0;\n" \
                       + "      break;\n"
    pkt_set_utils += "    default:\n" \
                   + "      *has_pkt_set_state = false;\n" \
                   + "      pkt_class = 1;\n" \
                   + "      break;\n" \
                   + "  }\n\n"
    pkt_set_utils += "  return pkt_class;\n}\n"


    # Output to files
    pkt_set_def_file = args.nf_path + "/pkt_set.h.gen"
    pkt_set_utils_file = args.nf_path + "/pkt_set_utils.c.gen"
    with open(pkt_set_def_file, "w") as f:
        f.write("#pragma once\n\n")
        f.write("//rss hash functions\n")
        f.write(f"#define RSS_HFS {{{', '.join(rss_hf_by_nic)}}}\n")
        f.write(f"#define RSS_SYMMETRIC {rss_symmetric}\n\n")
        f.write(pkt_set_def)

    with open(pkt_set_utils_file, "w") as f:
        f.write(pkt_set_utils)
