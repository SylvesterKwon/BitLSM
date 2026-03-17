import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt

# 1. 데이터 불러오기 (파일 경로를 넣어주세요)
df = pd.read_csv('~/workspace/BitLSM/experiment_results/latency-log.csv')

# 2. GET 작업만 필터링
df_get = df[df['operation_type'] == 'GET']

# 3. 박스 플롯 시각화 (라벨별 비교)
plt.figure(figsize=(10, 6))
sns.boxplot(x='experiment_label', y='latency_us', data=df_get)
plt.title('DB Latency by Experiment Label (GET only)')
plt.ylabel('Latency (us)')
plt.yscale('log')
plt.xlabel('Experiment Label')
plt.grid(axis='y', linestyle='--', alpha=0.7)
plt.show()

# 4. (추가) 주요 통계량 확인 (P95, P99 등)
stats = df_get.groupby('experiment_label')['latency_us'].agg(['mean', 'median', 'max', lambda x: x.quantile(0.95), lambda x: x.quantile(0.99)])
stats.columns = ['Mean', 'Median', 'Max', 'P95', 'P99']

plt.savefig('latency_log_scale-xx.png', dpi=300)