<?php
function timeToId($ts) {
    if(!is_numeric($ts) || $ts < 0)
    {
        $ts = $ts->getTimestamp();
    }

    // turn it into hex
    $hexTs = dechex($ts);
    // pad it out to 8 chars
    $hexTs = str_pad($hexTs, 8, "0", STR_PAD_LEFT);
    // make an _id from it
    return new MongoId($hexTs."0000000000000000");
}

$realTimeZone = 'America/Toronto';
date_default_timezone_set($realTimeZone);

$mongo = new MongoClient("mongodb://teeeee:27017");
$events = $mongo->te->events;

$begin = new DateTime("today");
$begin->modify("-2 weeks");
$begin->modify("+20 hour");

$begin = timeToId($begin);

$it = $events->find(['_id' => ['$gte' => $begin]]);

$cur = null;
$asleep = 0;

$dataTable =
[
    'cols' =>
    [
        [ 'type' => 'string', 'id' => 'Position'],
        [ 'type' => 'string', 'id' => 'Name'],
        [ 'type' => 'date', 'id' => 'Start'],
        [ 'type' => 'date', 'id' => 'End']
    ],
    'rows' => []
];

const NOD_OFF_TIME = 30 * 60;

function saveSleepInterval($begin, $end)
{
    global $cur;
    global $days;

    global $dataTable;

    $key = $cur;

    $key = strftime("%Y-%m-%d", $key);

    if(!isset($days[$key]))
    {
        // $days[$key] = [];
    }

    $dataTable['rows'][] =
    [
        'c' =>
        [
            [ 'v' => $key ],
            [ 'v' => "" ],
            [ 'v' => $begin - $cur + 60 * 60],
            [ 'v' => $end - $cur + 60 * 60 ]
        ]
    ];

    // $days[$key][] = [ strftime("%I:%M %p", $begin), strftime("%I:%M %p", $end) ];
}

while($it->hasNext())
{
    $rec = $it->next();

    $d = $rec['_id']->getTimestamp();

    if(is_null($cur))
    {
        $cur = strftime("%Y-%m-%d", $d);

        $bedTime = new DateTime($cur);
        $bedTime->modify("+20 hour");
        $bedTime = $bedTime->getTimestamp();

        $wakeUp = new DateTime($cur);
        $wakeUp->modify("+32 hour");
        $wakeUp = $wakeUp->getTimestamp();

        $asleep = $bedTime + NOD_OFF_TIME;
        $cur = strtotime($cur . " 00:00:00 UTC");
    }

    if($d < $bedTime)
    {
        continue;
    }

    if($d > $wakeUp)
    {
        if($asleep < $wakeUp)
        {
            saveSleepInterval($asleep, $wakeUp);
        }

        $cur = null;
        continue;
    }

    if($asleep < $d)
    {
        saveSleepInterval($asleep, $d);
    }
    
    $asleep = $d + NOD_OFF_TIME;
}

$dataTable['rows'] = array_reverse($dataTable['rows']); 
// echo json_encode($days, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES)

?>
<html>
<head><title>TeeEee</title></head>
<script type="text/javascript" src="https://www.google.com/jsapi"></script>
    <script type="text/javascript">
      google.load("visualization", "1", {packages:["timeline"]});
      google.setOnLoadCallback(drawChart);

      function drawChart() {
        var container = document.getElementById('timeline');
        var chart = new google.visualization.Timeline(container);
        var data = <?php echo json_encode($dataTable) ?>;

        for(var r in data.rows)
        {
            for(var c in data.rows[r].c)
            {
                if(c > 1)
                {
                    var v = new Date(data.rows[r].c[c].v * 1000);
                    data.rows[r].c[c].v = v;
                }
            }
        }

        var dataTable = new google.visualization.DataTable(data);

        var options = { timeline: { singleColor: '#434CD1' } };
        chart.draw(dataTable, options);
      }
    </script>
  </head>
<body>
    <div id="timeline" style="width: 1000px; height: 512px;"></div>
</body>
</html>