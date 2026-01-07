import pandas as pd
import matplotlib.pyplot as plt

# 1. 파일 경로 및 이름 설정
file_paths = [
    '~/workspace/lsm-bitmap-index/experiment_results/no-si/no-si-write.csv',
    '~/workspace/lsm-bitmap-index/experiment_results/lu/lu-write.csv',
    '~/workspace/lsm-bitmap-index/experiment_results/ck/ck-write.csv',
]

names = [ 'NO-SI', 'LU', 'CK' ]

plt.figure(figsize=(6, 2.5))

for i, path in enumerate(file_paths):
    # CSV 읽기
    df = pd.read_csv(path)
    
    # [전처리] 
    start_time = df['time_elapsed_us'].iloc[0]
    df['relative_time_us'] = df['time_elapsed_us'] - start_time
    df['relative_time_s'] = df['relative_time_us'] / 1_000_000
    df['cumulative_count'] = range(1, len(df) + 1)
    
    # --- [핵심 최적화 부분] ---
    # 데이터가 너무 많으면(예: 10만 개 이상) 그래프 그릴 때 멈춥니다.
    # 전체 흐름을 보는 데 방해되지 않도록 일정 간격(step)으로 데이터를 솎아냅니다.
    
    total_rows = len(df)
    
    # 데이터가 10,000개 이상일 경우에만 샘플링 적용
    if total_rows > 10000:
        # 화면에 표시할 점의 개수를 대략 5,000개 정도로 제한 (조절 가능)
        step = total_rows // 5000 
        if step < 1: step = 1
        
        # 슬라이싱을 이용해 데이터 다운샘플링 (::step)
        # 마지막 데이터(최종 결과)는 포함하기 위해 tail 등 처리할 수도 있으나,
        # 단순 경향성 파악에는 이정도면 충분합니다.
        plot_data = df.iloc[::step]
    else:
        plot_data = df
        
    label_name = names[i]
    
    # marker='.'는 데이터가 많을 때 렌더링 부하가 큽니다. 
    # 선만 그려도 충분하다면 marker를 빼는 것이 가장 빠릅니다.
    # 여기서는 샘플링을 했으므로 마커를 유지해도 괜찮습니다.
    line, =plt.plot(plot_data['relative_time_s'], 
             plot_data['cumulative_count'], 
             label=label_name, 
             marker='.', 
             markersize=1, 
             linewidth=1) 
    
    # [수정 2] 끝난 지점에 시간(초) 마커 표시
    # 마지막 데이터 포인트 추출
    last_x = plot_data['relative_time_s'].iloc[-1]
    last_y = plot_data['cumulative_count'].iloc[-1]
    line_color = line.get_color() # 그래프와 같은 색 사용
    
    # 끝점에 눈에 띄는 점 하나 찍기
    plt.plot(last_x, last_y, marker='x', markersize=6, color=line_color)

    plt.annotate(f'{last_x:.1f}s', 
             xy=(last_x, last_y),           # 기준점 (데이터 좌표)
             xytext=(0, 10),                # [핵심] 기준점에서 (x, y)만큼 픽셀/포인트 이동 (여기서 y를 50 줌)
             textcoords='offset points',    # 위 좌표를 데이터 값이 아닌 '포인트(화면 거리)'로 인식하게 함
             fontsize=6, 
             color=line_color, 
             fontweight='bold', 
             ha='center',                     # horizontalalignment 줄임말
             va='center',                   # verticalalignment 줄임말
             clip_on=False)
    

    print(f"[{label_name}] Total: {total_rows} -> Last Time: {last_x:.2f}s")
    print(f"[{label_name}] Total: {total_rows} -> Plotting: {len(plot_data)} points")

plt.xlabel('Time Elapsed (s)', fontsize=8)
plt.ylabel('Cumulative Rows Inserted', fontsize=8)
plt.tick_params(axis='both', labelsize=8)

# 지수 표기(1e8 등) 크기 조절
ax = plt.gca() # 현재 축 객체 가져오기
ax.yaxis.get_offset_text().set_fontsize(8) # Y축 지수 폰트 줄임
ax.xaxis.get_offset_text().set_fontsize(8) # X축 지수 폰트 줄임 (필요 시)

plt.grid(True, linestyle='--', alpha=0.6)
plt.legend(fontsize=8, loc='upper left') 

plt.tight_layout()

import os
output_path = os.path.expanduser('~/workspace/lsm-bitmap-index/experiment_results/db_insert_throughput_comparison.png')
plt.savefig(output_path, dpi=300)
print(f"Saved graph to {output_path}")

plt.show()