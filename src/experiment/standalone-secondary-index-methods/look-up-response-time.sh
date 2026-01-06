#!/bin/bash

# 1. 실행 파일 및 경로 설정
BIN_PATH="$HOME/workspace/lsm-bitmap-index/build/bin/test"
DATA_DIR="/scratch/data/standalone-si-test"
RESULT_DIR="$HOME/workspace/lsm-bitmap-index/experiment_results"

# 2. 실험 파라미터 설정
# BASELINES=("PF_EU" "PF_LU" "PF_CK" "IM_EU" "IM_LU" "IM_CK" "NO_SI")
BASELINES=("PF_LU")
R=0
WRITE_N=10000000  # 1e7
READ_N=100000 # 1e4
PAYLOAD_SIZE=256

# 3. 결과 저장 폴더가 없으면 생성
mkdir -p "$RESULT_DIR"

# 4. 루프 시작
for B in "${BASELINES[@]}"; do
    echo "==================================================================="
    echo "[Start] Algo: $B | Write N: $WRITE_N | Read N: $READ_N | Ratio: $R"
    echo "==================================================================="
    rm -rf "$DATA_DIR"

    echo "MAKING $N DATA..."
    $BIN_PATH \
        -b "$B" \
        -n "$WRITE_N" \
        -r "0" \
        -d "$DATA_DIR" \
        -o "$RESULT_DIR" \
        -p "$PAYLOAD_SIZE"


    echo "READING $READ_N DATA..."
    $BIN_PATH \
        -b "$B" \
        -n "$READ_N" \
        -r "1" \
        -d "$DATA_DIR" \
        -o "$RESULT_DIR" \
        -p "$PAYLOAD_SIZE"
    
    # 실행 결과 확인
    if [ $? -eq 0 ]; then
        echo "[Done] Experiment finished successfully."
    else
        echo "[Error] Experiment failed!"
        exit 1
    fi

    sleep 2
done

echo "All experiments completed!"