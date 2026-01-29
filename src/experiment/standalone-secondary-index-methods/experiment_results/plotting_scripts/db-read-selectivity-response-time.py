import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os
from matplotlib.lines import Line2D # 범례 생성을 위한 도구

# --- [사용자 제공 코드] ---
file_paths = [
    '~/workspace/lsm-bitmap-index/src/experiment/standalone-secondary-index-methods/experiment_results/no-si/no-si-read.csv',
    '~/workspace/lsm-bitmap-index/src/experiment/standalone-secondary-index-methods/experiment_results/ck/ck-pf-read.csv',
    '~/workspace/lsm-bitmap-index/src/experiment/standalone-secondary-index-methods/experiment_results/ck/ck-im-read.csv',
    '~/workspace/lsm-bitmap-index/src/experiment/standalone-secondary-index-methods/experiment_results/lu/lu-pf-read.csv',
    '~/workspace/lsm-bitmap-index/src/experiment/standalone-secondary-index-methods/experiment_results/lu/lu-im-read.csv',
]

names = [ 'NO-SI', 'CK-PF', 'CK-IM', 'LU-PF', 'LU-IM' ]

colors = sns.color_palette('hls', 4)
color_map = {
    'NO-SI': '#AEAEAE',
    'CK-PF': colors[0],
    'CK-IM': colors[1],
    'LU-PF': colors[2],
    'LU-IM': colors[3],
}
# -----------------------

all_data = []

for i, path in enumerate(file_paths):
    try:
        full_path = os.path.expanduser(path)
        df = pd.read_csv(full_path)
        
        # [전처리] 
        # Selectivity가 X축, Response Time이 Y축
        # 보기 편하게 ms 단위로 변환
        df['response_time_ms'] = df['response_time_us'] / 1_000
        df['Method'] = names[i]
        
        all_data.append(df)
    except FileNotFoundError:
        print(f"Warning: File not found at {path}")
        continue

if not all_data:
    print("No data loaded.")
    exit()

combined_df = pd.concat(all_data)

# ### [핵심 수정] 데이터 섞기 (Shuffling) ###
# frac=1: 전체 데이터를 100% 샘플링(즉, 순서만 섞음)
# random_state=42: 매번 똑같이 섞이도록 고정
combined_df = combined_df.sample(frac=1, random_state=42).reset_index(drop=True)
# ----------------------------------------

# 그래프 크기 설정
plt.figure(figsize=(6, 3))

# 1. [수정] Scatter Plot (데이터 점)
# s=1: 점 크기 1px
# alpha=1.0: 투명도 없음 (완전 불투명)
sns.scatterplot(data=combined_df, 
                x='selectivity', y='response_time_ms', 
                hue='Method', palette=color_map,
                alpha=0.5, s=1, edgecolor=None, legend=False, zorder=1)

# 3. [축 설정] Log-Log Scale
plt.xscale('log')
plt.yscale('log')

# --- 스타일 및 범례 설정 ---
plt.xlabel('Rows Selected', fontsize=8)
plt.ylabel('Response Time (ms)', fontsize=8)

# 눈금 및 그리드
plt.grid(True, which="major", ls="-", alpha=0.5) 
plt.grid(True, which="minor", ls=":", alpha=0.3) 
plt.tick_params(axis='both', which='major', labelsize=8)

# [수정] 범례 수동 생성 (점선으로 표시)
legend_elements = [
    Line2D([0], [0], color=color_map[name], lw=2, 
           linestyle='-', label=name) # 범례도 점선으로 맞춤
    for name in names
]
plt.legend(handles=legend_elements, loc='upper left', fontsize=8, framealpha=0.5)

plt.tight_layout()

# 저장
output_dir = os.path.expanduser('~/workspace/lsm-bitmap-index/src/experiment/standalone-secondary-index-methods/experiment_results')
if not os.path.exists(output_dir):
    os.makedirs(output_dir, exist_ok=True)

output_path = os.path.join(output_dir, 'db_read_selectivity_response_time.png')
plt.savefig(output_path, dpi=300, bbox_inches='tight')
print(f"Saved graph to {output_path}")

plt.show()