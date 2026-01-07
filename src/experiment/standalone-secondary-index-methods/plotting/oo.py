import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

# 1. 데이터 로드
df = pd.read_csv('~/workspace/lsm-bitmap-index/experiment_results/latency-log.csv')

# 2. GET 작업 필터링 및 전처리
df['operation_type'] = df['operation_type'].str.strip()
df_get = df[df['operation_type'] == 'GET'].copy()

# 로그 스케일 적용을 위해 0 이하의 값은 아주 작은 양수로 치환 (로그는 0 이하에서 정의되지 않음)
df_get['latency_us'] = df_get['latency_us'].clip(lower=1)

# 3. 시각화
plt.figure(figsize=(10, 6))
sns.set_theme(style="whitegrid")

# 박스 플롯과 스트립 플롯 병행
# whis=[5, 95] 설정을 통해 아주 극단적인 값 외의 분포 범위를 박스로 표시
ax = sns.boxplot(
    data=df_get, 
    x='experiment_label', 
    y='latency_us', 
    palette='Set2',
    showmeans=True,  # 평균값 표시
    meanprops={"marker":"o", "markerfacecolor":"white", "markeredgecolor":"black", "markersize":"7"}
)

# 실제 점들을 찍어서 튀는 값들의 양을 확인 (alpha로 투명도 조절)
sns.stripplot(data=df_get, x='experiment_label', y='latency_us', color="black", alpha=0.15, size=3)

# --- 핵심: Y축 로그 스케일 설정 ---
plt.yscale('log')

# Y축 숫자가 10^n 형태가 아닌 일반 숫자로 나오도록 설정
ax.yaxis.set_major_formatter(ScalarFormatter())
ax.set_yticks([1, 10, 100, 1000, 10000]) # 데이터 범위에 따라 적절히 조절

plt.title('Latency Distribution (Log Scale) - GET Operation', fontsize=15)
plt.xlabel('Experiment Label', fontsize=12)
plt.ylabel('Latency ($\mu s$) - Log Scale', fontsize=12)

plt.tight_layout()
plt.savefig('latency_log_scale.png', dpi=300)
plt.show()