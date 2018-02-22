# koko
The Koko stack provides all the basic software for running and using a Koko

Development environment setup (Ubuntu 16.0.4 w/ ROS Kinetic):

```bash
mkdir -p ~/koko_ws/src && cd "$_"
catkin_init_workspace
git clone https://github.com/brentyi/koko.git
rosdep install --from-paths src --ignore-src -r -y
catkin_make install
```