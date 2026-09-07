always use mise for tooling  
uv for pyhon environment  
always run python as 'uv run python'  
everytime you need to run bash or python code, **always do it from a file based code**, put it under ./scripts or ./script-helpers if too ephemeral, used only once  
keep all tasks with help documenter in make, just, mise tasks

always do small semantic commits

always ensure proper access to espressif tools: it is solved under /W/NVT/espnow-master-slave  
see 00-SOURCE-this.sh