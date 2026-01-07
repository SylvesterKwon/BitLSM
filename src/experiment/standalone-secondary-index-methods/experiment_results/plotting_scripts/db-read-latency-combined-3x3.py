import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os
from matplotlib.lines import Line2D

# 1. 공통 설정 (파일 경로 및 색상)
file_paths = [
    '~/workspace/lsm-bitmap-index/experiment_results/no-si/no-si-read.csv',
    '~/workspace/lsm-bitmap-index/experiment_results/ck/ck-pf-read.csv',
    '~/workspace/lsm-bitmap-index/experiment_results/ck/ck-im-read.csv',
    '~/workspace/lsm-bitmap-index/experiment_results/lu/lu-pf-read.csv',
    '~/workspace/lsm-bitmap-index/experiment_results/lu/lu-im-read.csv',
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

# 2. 데이터 로드 (공통)
all_data = []
for i, path in enumerate(file_paths):
    try:
        full_path = os.path.expanduser(path)
        df = pd.read_csv(full_path)
        # 단위 변환
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

# ---------------------------------------------------------
# 그래프 그리기 시작 (1행 2열 구조, 전체 크기 6x3인치)
# ---------------------------------------------------------
fig, axes = plt.subplots(1, 2, figsize=(6, 3))

# =========================================================
# [왼쪽 그래프] 1번: Box Plot
# =========================================================
ax1 = axes[0] # 첫 번째 칸

sns.boxplot(x='Method', y='response_time_ms', data=combined_df, 
            width=0.5, linewidth=1, palette=color_map, 
            hue='Method', showfliers=True, ax=ax1) # ax=ax1 추가

# 스타일 설정 (plt. -> ax1. 으로 변경)
ax1.set_xlabel('Method', fontsize=8)
ax1.set_ylabel('Response Time (ms)', fontsize=8)
ax1.set_yscale('log')

# 지수 표기 크기 조절
ax1.yaxis.get_offset_text().set_fontsize(8)
# ax1.xaxis는 카테고리라 지수 표기 없음

# 눈금 조절
ax1.tick_params(axis='both', which='major', labelsize=8)
ax1.grid(True, axis='y', linestyle='--', alpha=0.6)

# =========================================================
# [오른쪽 그래프] 2번: Scatter Plot
# =========================================================
ax2 = axes[1] # 두 번째 칸

# 데이터 섞기 (Scatter Plot용)
shuffled_df = combined_df.sample(frac=1, random_state=42).reset_index(drop=True)

sns.scatterplot(data=shuffled_df, 
                x='selectivity', y='response_time_ms', 
                hue='Method', palette=color_map,
                alpha=0.5, s=1, edgecolor=None, legend=False, 
                zorder=1, ax=ax2) # ax=ax2 추가

# 스타일 설정
ax2.set_xscale('log')
ax2.set_yscale('log')
ax2.set_xlabel('Rows Selected', fontsize=8)
ax2.set_ylabel('Response Time (ms)', fontsize=8)

# 눈금 및 그리드
ax2.grid(True, which="major", ls="-", alpha=0.5) 
ax2.grid(True, which="minor", ls=":", alpha=0.3) 
ax2.tick_params(axis='both', which='major', labelsize=8)

# 범례 수동 생성
legend_elements = [
    Line2D([0], [0], color=color_map[name], lw=2, 
           linestyle='-', label=name)
    for name in names
]
ax2.legend(handles=legend_elements, loc='upper left', fontsize=8, framealpha=0.5)

# ---------------------------------------------------------
# 마무리 및 저장
# ---------------------------------------------------------
ax1.set_title('(a) 응답 시간 분포', fontsize=8)
ax2.set_title('(b) 선택도에 따른 응답 시간', fontsize=8)
plt.tight_layout()

output_dir = os.path.expanduser('~/workspace/lsm-bitmap-index/experiment_results/')
if not os.path.exists(output_dir):
    os.makedirs(output_dir, exist_ok=True)

output_path = os.path.join(output_dir, 'db_read_latency_combined_3x3.png')
plt.savefig(output_path, dpi=300, bbox_inches='tight')
print(f"Saved combined graph to {output_path}")

plt.show()