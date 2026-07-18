#!/bin/bash

echo "🚀 正在为您一键拉起 Embodied-SimLite 极速孪生环境与导航栈..."

# 1. 启动 Web 物理引擎 (终端 1)
# 挂起 3 秒以确保服务端优先启动，避免桥接器连接失败
gnome-terminal --tab --title="1-WebEngine" -- bash -c "cd ~/embodied-sim-lite && python3 ProductV1.0.py; exec bash"
sleep 3 

# 2. 启动 ROS 2 桥接器 (终端 2)
gnome-terminal --tab --title="2-Bridge" -- bash -c "cd ~/embodied-sim-lite && source /opt/ros/humble/setup.bash && python3 sim_ros2_bridgeV1.0.py; exec bash"

# 3. 启动 SLAM 建图 (终端 3)
gnome-terminal --tab --title="3-SLAM" -- bash -c "source /opt/ros/humble/setup.bash && ros2 launch slam_toolbox online_async_launch.py use_sim_time:=false; exec bash"

# 4. 启动 Nav2 导航栈 (终端 4)
# 注意：这里已经为你加上了调优过的 nav2_params.yaml
gnome-terminal --tab --title="4-Nav2" -- bash -c "source /opt/ros/humble/setup.bash && ros2 launch nav2_bringup navigation_launch.py use_sim_time:=false params_file:=/home/yahboom/nav2_params.yaml; exec bash"

# 5. 启动 RViz2 (终端 5)
gnome-terminal --tab --title="5-RViz2" -- bash -c "source /opt/ros/humble/setup.bash && rviz2 -d /opt/ros/humble/share/nav2_bringup/rviz/nav2_default_view.rviz; exec bash"

echo "✅ 5 个核心基础节点已全部在后台标签页启动！"
echo "🎮 请在此终端继续执行遥控节点或打开浏览器访问 http://localhost:8000"