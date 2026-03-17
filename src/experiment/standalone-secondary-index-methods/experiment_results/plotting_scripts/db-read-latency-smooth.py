import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
from scipy.ndimage import gaussian_filter1d # 핵심: 곡선을 부드럽게 만드는 필터
import os

file_paths = [
    '~/workspace/BitLSM/experiment_results/no-si/no-si-read.csv',
    '~/workspace/BitLSM/experiment_results/ck/ck-pf-read.csv',
    '~/workspace/BitLSM/experiment_results/ck/ck-im-read.csv',
    '~/workspace/BitLSM/experiment_results/lu/lu-pf-read.csv',
    '~/workspace/BitLSM/experiment_results/lu/lu-im-read.csv',
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

# 2. 데이터 로드
all_data = []
for i, path in enumerate(file_paths):
    try:
        full_path = os.path.expanduser(path)
        df = pd.read_csv(full_path)
        df['response_time_ms'] = df['response_time_us'] / 1_000
        df['Method'] = names[i]
        all_data.append(df)
    except FileNotFoundError:
        continue

if not all_data:
    print("No data loaded.")
    exit()

combined_df = pd.concat(all_data)

# --- [핵심 로직] 부드러운 곡선 그리기 함수 ---
def plot_smooth_band(ax, x, y, label, color, sigma=2, bins=1000):
    """
    x, y: 원본 데이터
    sigma: 스무딩 강도 (클수록 더 부드러워짐)
    bins: 데이터를 쪼갤 구간 수 (로그 스케일 기준)
    """
    # 1. X축이 로그 스케일이므로, 로그 간격으로 bin을 생성합니다.
    min_x, max_x = x.min(), x.max()
    if min_x <= 0: min_x = 1e-6 # 0 방지
    
    # 로그 스케일로 등간격 구간 생성
    bin_edges = np.logspace(np.log10(min_x), np.log10(max_x), num=bins)
    
    # 각 구간에 속하는 데이터의 평균(mean)과 표준편차(std) 계산
    # digitize를 이용해 각 데이터가 몇 번째 구간인지 찾음
    bin_indices = np.digitize(x, bin_edges)
    
    bin_centers = []
    bin_means = []
    bin_stds = []
    
    for i in range(1, len(bin_edges)):
        # 해당 구간에 있는 데이터 추출
        mask = bin_indices == i
        if np.any(mask):
            current_y = y[mask]
            bin_centers.append(np.sqrt(bin_edges[i-1] * bin_edges[i])) # 기하 평균 중심
            bin_means.append(current_y.mean())
            bin_stds.append(current_y.std())
            
    # 배열로 변환
    bin_centers = np.array(bin_centers)
    bin_means = np.array(bin_means)
    bin_stds = np.array(bin_stds)
    
    # 데이터가 너무 적으면 스무딩 없이 그리기
    if len(bin_centers) < 4:
        ax.plot(bin_centers, bin_means, label=label, color=color, linewidth=1, linestyle='--')
        return

    # 2. Gaussian Smoothing 적용 (부드럽게 만들기)
    smooth_mean = gaussian_filter1d(bin_means, sigma=sigma)
    smooth_std = gaussian_filter1d(bin_stds, sigma=sigma)
    
    # 3. 그래프 그리기
    # 중심선 (평균)
    ax.plot(bin_centers, smooth_mean, color=color, label=label, linewidth=1, linestyle='--')
    
    # 음영 영역 (평균 - 표준편차 ~ 평균 + 표준편차)
    ax.fill_between(bin_centers, 
                    np.maximum(0, smooth_mean - smooth_std), # 0 이하로 내려가는 것 방지
                    smooth_mean + smooth_std, 
                    color=color, alpha=0.2, edgecolor=None) # 테두리 없음

# ---------------------------------------------

plt.figure(figsize=(7, 5))
ax = plt.gca()

# 각 Method별로 스무딩 함수 호출
for name in names:
    method_data = combined_df[combined_df['Method'] == name]
    if len(method_data) == 0: continue
    
    # X, Y 데이터 추출
    x_data = method_data['selectivity']
    y_data = method_data['response_time_ms']
    
    # 커스텀 함수로 그리기
    # bins=200: 가로 해상도 (높을수록 디테일함)
    # sigma=2: 부드러움 정도 (높을수록 뭉개짐)
    plot_smooth_band(ax, x_data, y_data, 
                     label=name, color=color_map[name], 
                     bins=150, sigma=2.0)

# 축 설정
plt.xscale('log')
plt.yscale('log') # Y축도 로그 스케일 (필요 시 주석 처리)

plt.xlabel('Selectivity (Rows Selected)', fontsize=10)
plt.ylabel('Response Time (ms)', fontsize=10)
plt.grid(True, which="major", ls="-", alpha=0.4)
plt.grid(True, which="minor", ls=":", alpha=0.2)

# 범례
plt.legend(title='Method', fontsize=9, loc='upper left')

plt.tight_layout()

# 저장
output_dir = os.path.expanduser('~/workspace/BitLSM/experiment_results/')
if not os.path.exists(output_dir):
    os.makedirs(output_dir, exist_ok=True)
output_path = os.path.join(output_dir, 'db_read_latency_smooth.png')
plt.savefig(output_path, dpi=300, bbox_inches='tight')

print(f"Saved smooth graph to {output_path}")
plt.show()