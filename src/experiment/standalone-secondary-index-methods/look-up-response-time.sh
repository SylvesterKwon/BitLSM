#!/bin/bash

# 1. 실행 파일 및 경로 설정
BIN_PATH="$HOME/workspace/lsm-bitmap-index/build/bin/test"
NO_SI_DATA_DIR="/scratch/data/no-si"
LU_DATA_DIR="/scratch/data/lu"
CK_DATA_DIR="/scratch/data/ck"

NO_SI_RESULT_DIR="$HOME/workspace/lsm-bitmap-index/experiment_results/no-si"
LU_RESULT_DIR="$HOME/workspace/lsm-bitmap-index/experiment_results/lu"
CK_RESULT_DIR="$HOME/workspace/lsm-bitmap-index/experiment_results/ck"

# 공통 실험 파라미터 설정
WRITE_N=100000000  # 1e8
READ_N=10000 # 1e4
PAYLOAD_SIZE=256

mkdir -p "$NO_SI_RESULT_DIR"
mkdir -p "$LU_RESULT_DIR"
mkdir -p "$CK_RESULT_DIR"

################################# NO_SI ######################################
# echo "[NO_SI] MAKING $WRITE_N DATA... ----------------------------------------------"
# rm -rf "$NO_SI_DATA_DIR"
# $BIN_PATH \
#     -b "NO_SI" \
#     -n "$WRITE_N" \
#     -r "0" \
#     -d "$NO_SI_DATA_DIR" \
#     -o "$NO_SI_RESULT_DIR" \
#     -p "$PAYLOAD_SIZE" \
#     -l "no-si-write"

echo "[NO_SI] READING 10 DATA... (NO SI)------------------------------"
$BIN_PATH \
    -b "NO_SI" \
    -n "100" \
    -r "1" \
    -d "$NO_SI_DATA_DIR" \
    -o "$NO_SI_RESULT_DIR" \
    -p "$PAYLOAD_SIZE" \
    -l "no-si-read"

################################# LU ######################################
# echo "[LU] MAKING $WRITE_N DATA... ----------------------------------------------"
# rm -rf "$LU_DATA_DIR"
# $BIN_PATH \
#     -b "PF_LU" \
#     -n "$WRITE_N" \
#     -r "0" \
#     -d "$LU_DATA_DIR" \
#     -o "$LU_RESULT_DIR" \
#     -p "$PAYLOAD_SIZE" \
#     -l "lu-write"


echo "[LU] READING $READ_N DATA... (Post Filtering)------------------------------"
$BIN_PATH \
    -b "PF_LU" \
    -n "$READ_N" \
    -r "1" \
    -d "$LU_DATA_DIR" \
    -o "$LU_RESULT_DIR" \
    -p "$PAYLOAD_SIZE" \
    -l "lu-pf-read"


echo "[LU] READING $READ_N DATA... (Index Merging)-------------------------------"
$BIN_PATH \
    -b "IM_LU" \
    -n "$READ_N" \
    -r "1" \
    -d "$LU_DATA_DIR" \
    -o "$LU_RESULT_DIR" \
    -p "$PAYLOAD_SIZE" \
    -l "lu-im-read"

################################# CK ######################################
# echo "[CK] MAKING $WRITE_N DATA... ----------------------------------------------"
# rm -rf "$CK_DATA_DIR"
# $BIN_PATH \
#     -b "PF_CK" \
#     -n "$WRITE_N" \
#     -r "0" \
#     -d "$CK_DATA_DIR" \
#     -o "$CK_RESULT_DIR" \
#     -p "$PAYLOAD_SIZE" \
#     -l "ck-write"

echo "[CK] READING $READ_N DATA... (Post Filtering)------------------------------"
$BIN_PATH \
    -b "PF_CK" \
    -n "$READ_N" \
    -r "1" \
    -d "$CK_DATA_DIR" \
    -o "$CK_RESULT_DIR" \
    -p "$PAYLOAD_SIZE" \
    -l "ck-pf-read"


echo "[CK] READING $READ_N DATA... (Index Merging)-------------------------------"
$BIN_PATH \
    -b "IM_CK" \
    -n "$READ_N" \
    -r "1" \
    -d "$CK_DATA_DIR" \
    -o "$CK_RESULT_DIR" \
    -p "$PAYLOAD_SIZE" \
    -l "ck-im-read"


echo "All experiments completed!"