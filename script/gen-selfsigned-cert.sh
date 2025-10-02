#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
用法: gen-selfsigned-cert.sh [-d 有效天数] [-C 国家] [-ST 省份] [-L 城市] [-O 组织] [-OU 部门] [-CN 通用名] [-E 邮箱] [-o 输出目录]

示例:
  ./script/gen-selfsigned-cert.sh -o certs -CN example.com

生成结果:
  certs/server.key  (私钥)
  certs/server.crt  (自签证书)
EOF
}

DAYS=365
COUNTRY="CN"
STATE="Beijing"
LOCALITY="Beijing"
ORG="co_wq"
ORG_UNIT="Dev"
COMMON_NAME="localhost"
EMAIL="admin@example.com"
OUTDIR="certs"

while [[ $# -gt 0 ]]; do
    case "$1" in
    -d|--days)
        DAYS="$2"; shift 2;;
    -C|--country)
        COUNTRY="$2"; shift 2;;
    -ST|--state)
        STATE="$2"; shift 2;;
    -L|--locality)
        LOCALITY="$2"; shift 2;;
    -O|--org)
        ORG="$2"; shift 2;;
    -OU|--org-unit)
        ORG_UNIT="$2"; shift 2;;
    -CN|--common-name)
        COMMON_NAME="$2"; shift 2;;
    -E|--email)
        EMAIL="$2"; shift 2;;
    -o|--outdir)
        OUTDIR="$2"; shift 2;;
    -h|--help)
        usage; exit 0;;
    *)
        echo "未知参数: $1" >&2
        usage; exit 1;;
    esac
done

mkdir -p "$OUTDIR"

KEY_PATH="$OUTDIR/server.key"
CRT_PATH="$OUTDIR/server.crt"

SUBJECT="/C=$COUNTRY/ST=$STATE/L=$LOCALITY/O=$ORG/OU=$ORG_UNIT/CN=$COMMON_NAME/emailAddress=$EMAIL"

openssl req -x509 -nodes -newkey rsa:2048 \
    -keyout "$KEY_PATH" \
    -out "$CRT_PATH" \
    -days "$DAYS" \
    -subj "$SUBJECT"

echo "生成成功:"
echo "  私钥: $KEY_PATH"
echo "  证书: $CRT_PATH"

