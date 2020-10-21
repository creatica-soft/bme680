<?php
$height = "-h 100";
$width = "-w 1000";
$start = "-s end-1w";
$end = "-e now";
$file_t = "/opt/nginx/html/images/temperature.png";
$file_h = "/opt/nginx/html/images/humidity.png";
$file_p = "/opt/nginx/html/images/pressure.png";
$file_a = "/opt/nginx/html/images/iaq.png";
$file_as = "/opt/nginx/html/images/iaqs.png";
$file_ap = "/opt/nginx/html/images/iaqp.png";
$file_co = "/opt/nginx/html/images/co2.png";
$file_bvoc = "/opt/nginx/html/images/bvoc.png";
$def_t = "DEF:t=/var/rrd/env.rrd:temperature:AVERAGE";
$def_h = "DEF:h=/var/rrd/env.rrd:humidity:AVERAGE";
$def_p = "DEF:p=/var/rrd/env.rrd:pressure:AVERAGE";
$def_a = "DEF:a=/var/rrd/env.rrd:iaq:AVERAGE";
$def_as = "DEF:as=/var/rrd/env.rrd:iaqs:AVERAGE";
$def_ap = "DEF:ap=/var/rrd/env.rrd:iaqp:AVERAGE";
$def_co = "DEF:ap=/var/rrd/env.rrd:co2eq:AVERAGE";
$def_bvoc = "DEF:ap=/var/rrd/env.rrd:bvoc:AVERAGE";
$line_t = "LINE1:t#0000FF:'temperature'";
$line_h = "LINE1:h#0000FF:'humidity'";
$line_p = "LINE1:p#0000FF:'pressure'";
$line_a = "LINE1:a#0000FF:'iaq'";
$line_as = "LINE1:as#0000FF:'static iaq'";
$line_ap = "LINE1:ap#0000FF:'air pollution'";
$line_co = "LINE1:ap#0000FF:'CO2 equiv.'";
$line_bvoc = "LINE1:ap#0000FF:'bVOC'";
$values = [$end, $start, $height, $width, "-t Temperature, deg C", $def_t, $line_t];
rrd_graph($file_t, $values);
print_r(rrd_error());
$values = [$end, $start, $height, $width, "-t Relative Humidity, %", $def_h, $line_h];
rrd_graph($file_h, $values);
print_r(rrd_error());
#$values = [$end, $start, $height, $width, "-l 990", "-u 1040", "-r", $def_p, $line_p];
$values = [$end, $start, $height, $width, "-t Atmospheric Pressure, mb(hPa)", "-A", "-Y", $def_p, $line_p];
rrd_graph($file_p, $values);
print_r(rrd_error());
$values = [$end, $start, $height, $width, "-t Index Air Quality", $def_a, $line_a];
rrd_graph($file_a, $values);
print_r(rrd_error());
$values = [$end, $start, $height, $width, "-t Static IAQ", $def_as, $line_as];
rrd_graph($file_as, $values);
print_r(rrd_error());
$values = [$end, $start, $height, $width, "-t Air Pollution, %", $def_ap, $line_ap];
rrd_graph($file_ap, $values);
print_r(rrd_error());
$values = [$end, $start, $height, $width, "-t CO2 Equivalent, ppm", $def_co, $line_co];
rrd_graph($file_co, $values);
print_r(rrd_error());
$values = [$end, $start, $height, $width, "-t Breath VOC Equivalent, ppm", $def_bvoc, $line_bvoc];
rrd_graph($file_bvoc, $values);
print_r(rrd_error());
?>
<html>
<head>
<meta http-equiv="Cache-control" content="no-store">
</head>
<body>
<img src="images/temperature.png" />
<br />
<img src="images/humidity.png" />
<br />
<img src="images/pressure.png" />
<br />
<img src="images/iaq.png" />
<br />
<img src="images/iaqs.png" />
<br />
<img src="images/iaqp.png" />
<br />
<img src="images/co2.png" />
<br />
<img src="images/bvoc.png" />
</body>
</html>
