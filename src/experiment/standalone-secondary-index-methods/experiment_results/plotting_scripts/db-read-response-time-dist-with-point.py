import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns # 박스플롯을 예쁘게 그리기 위해 사용
import os

# 1. 파일 경로 및 이름 설정
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

# 데이터를 하나로 합치기 위한 리스트
all_data = []

for i, path in enumerate(file_paths):
    try:
        full_path = os.path.expanduser(path)
        df = pd.read_csv(full_path)
        
        # [전처리] 단위 변환 (us -> ms)
        df['response_time_ms'] = df['response_time_us'] / 1_000
        
        # 구분자(Method) 열 추가
        df['Method'] = names[i]
        
        # 필요한 데이터만 가져오기
        all_data.append(df[['Method', 'response_time_ms']])
        
    except FileNotFoundError:
        print(f"Warning: File not found at {path}")
        continue

# 전체 데이터프레임 생성

combined_df = pd.concat(all_data)

plt.figure(figsize=(6, 3))

# --- [핵심] Box Plot 그리기 ---
# showfliers=False: 이상치(점)가 너무 많아 그래프가 납작해지면 이 옵션을 False로 끄거나 True로 켭니다.
# width=0.5: 박스 너비 조절
sns.boxplot(x='Method', y='response_time_ms', data=combined_df, 
            width=0.5, linewidth=1, palette=color_map, hue='Method', showfliers=True)

# --- [스타일 설정] ---
plt.xlabel('Method', fontsize=8)
plt.ylabel('Response Time (ms)', fontsize=8)
plt.yscale('log')  # Y축 로그 스케일

# 지수 표기(1e8 등) 크기 조절
ax = plt.gca() # 현재 축 객체 가져오기
ax.yaxis.get_offset_text().set_fontsize(8) # Y축 지수 폰트 줄임
ax.xaxis.get_offset_text().set_fontsize(8) # X축 지수 폰트 줄임 (필요 시)

# 눈금 숫자 크기 조절 (8pt)
plt.tick_params(axis='both', which='major', labelsize=8)

# 격자 (뒤쪽에 은은하게)
plt.grid(True, axis='y', linestyle='--', alpha=0.6)

plt.tight_layout()

# 저장
output_dir = os.path.expanduser('~/workspace/BitLSM/experiment_results/')
if not os.path.exists(output_dir):
    os.makedirs(output_dir, exist_ok=True)

output_path = os.path.join(output_dir, 'db_read_latency_boxplot.png')
plt.savefig(output_path, dpi=300, bbox_inches='tight')
print(f"Saved graph to {output_path}")

plt.show()