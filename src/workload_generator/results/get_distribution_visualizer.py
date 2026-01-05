import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os

# 1. 파일 설정 (저장된 경로에 맞게 수정하세요)
filename = 'set_distribution_test.csv'
script_dir = os.path.dirname(os.path.abspath(__file__))
csv_file_path = os.path.join(script_dir, filename)

try:
    df = pd.read_csv(csv_file_path)
    # [중요] 컬럼 이름 앞뒤 공백 제거 (에러 방지용)
    df.columns = df.columns.str.strip()
    print("로드된 컬럼:", df.columns.tolist())
    print(f"총 데이터 개수: {len(df)}건")
except Exception as e:
    print(f"CSV 로드 중 에러 발생: {e}")
    exit()

# 3. 그래프 그리기 (2x2 그리드)
fig, axes = plt.subplots(2, 2, figsize=(16, 12))
fig.suptitle(f'SET Workload Distribution Analysis (N={len(df)})', fontsize=20, weight='bold')

# ---------------------------------------------------------
# [좌상단] PK (Uniform Check)
# ---------------------------------------------------------
ax0 = axes[0, 0]
# 데이터가 너무 많으면 bin을 늘려서 봅니다.
sns.histplot(data=df, x='pk', bins=50, ax=ax0, color='skyblue', kde=False)
ax0.set_title('PK Distribution (Should be Flat/Uniform)', fontsize=14)
ax0.set_xlabel('Primary Key Value')
ax0.set_ylabel('Count')

# ---------------------------------------------------------
# [우상단] SK_1 (Binary: Gender etc.)
# ---------------------------------------------------------
ax1 = axes[0, 1]
# 0과 1의 개수 비율을 봅니다.
sns.countplot(data=df, x='sk_1', ax=ax1, palette='Set2')
ax1.set_title('SK_1 Distribution (Binary Ratio)', fontsize=14)
ax1.bar_label(ax1.containers[0]) # 막대 위에 숫자 표시

# ---------------------------------------------------------
# [좌하단] SK_2 (Range ~250: Nationality etc.)
# ---------------------------------------------------------
ax2 = axes[1, 0]
sns.histplot(data=df, x='sk_2', bins=250, ax=ax2, color='teal', kde=True)
ax2.set_title('SK_2 Distribution (Range 0~250)', fontsize=14)
ax2.set_xlabel('SK_2 Value')

# ---------------------------------------------------------
# [우하단] SK_3 (Zipfian: Income Level etc.)
# ---------------------------------------------------------
ax3 = axes[1, 1]
# 빈도수가 높은 순서대로 정렬해서 "L자 곡선"이 나오는지 확인합니다.
order = df['sk_3'].value_counts().index
sns.countplot(data=df, x='sk_3', order=order, ax=ax3, palette='magma')
ax3.set_title('SK_3 Zipfian Check (Sorted by Frequency)', fontsize=14)
ax3.set_xlabel('SK_3 Value (Ranked)')

# 4. 저장 및 출력
plt.tight_layout(rect=[0, 0.03, 1, 0.95])
output_filename = 'set_distribution_result.png'
plt.savefig(output_filename, dpi=300)
print(f"그래프가 '{output_filename}'으로 저장되었습니다.")
plt.show()

plt.savefig(os.path.join(script_dir, "set_distribution_result.png"))