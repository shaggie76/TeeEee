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

const DEBUG = 0;

$realTimeZone = 'America/Toronto';
date_default_timezone_set($realTimeZone);

$mongo = new MongoClient("mongodb://teeeee:27017");
$events = $mongo->te->events;

$begin = new DateTime("today");
$begin->modify("-2 weeks");
$begin->modify("+20 hour");

$begin = timeToId($begin);

$it = $events->find(['_id' => ['$gte' => $begin]])->sort(['_id' => 1]);

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

    if(DEBUG && !isset($days[$key]))
    {
        $days[$key] = [];
    }

    $dataTable['rows'][] =
    [
        'c' =>
        [
            [ 'v' => $key ],
            [ 'v' => "" ],
            [ 'v' => $begin - $cur ],
            [ 'v' => $end - $cur ]
        ]
    ];

    if(DEBUG)
    {
        $days[$key][] =
        [
            strftime("%I:%M %p", $begin),
            strftime("%I:%M %p", $end),
            $begin - $cur,
            $end - $cur,
        ];
    }
}

function processDate($d, $t)
{
    global $cur;
    global $bedTime;
    global $wakeUp;
    global $asleep;

    if(!is_null($cur) && ($d > $wakeUp))
    // if(!is_null($cur) && (($d > $wakeUp) || ($t === 'stop')))
    {
        if($asleep < $wakeUp)
        {
            saveSleepInterval($asleep, $wakeUp);
        }

        $cur = null;
    }
    
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

        // $cur = strtotime($cur . " 00:00:00 UTC");
        $cur = strtotime($cur);
    }

    if($d < $bedTime)
    {
        return;
    }

    if($asleep < $d)
    {
        saveSleepInterval($asleep, $d);
    }
    
    $asleep = $d + NOD_OFF_TIME;
}

while($it->hasNext())
{
    $rec = $it->next();
    $d = $rec['_id']->getTimestamp();
    processDate($d, $rec['t']);
}

processDate(time(), 'stop');

$dataTable['rows'] = array_reverse($dataTable['rows']); 
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

        var today = new Date(0,0,0,0,0,0);

        for(var r in data.rows)
        {
            for(var c in data.rows[r].c)
            {
                if(c >= 2)
                {
                    var v = new Date(today);
                    v.setUTCSeconds(v.getUTCSeconds() + data.rows[r].c[c].v);
                    data.rows[r].c[c].v = v;
                }
            }
        }

        var dataTable = new google.visualization.DataTable(data);

        var options =
        {
            /* does not work
            hAxis :
            {
                minValue : new Date(0, 0, 0, 20, 0, 0),
                maxValue : new Date(0, 0, 1,  8, 0, 0)
            },
            */
            timeline :
            {
                singleColor: '#434CD1'
            }
        };
        chart.draw(dataTable, options);
      }
    </script>
  </head>
<body>
<?php
    $height = (count($dataTable['rows']) * 40) . "px";
    print("<div id=\"timeline\" style=\"width: 900px; height: $height;\"></div>\n");
    if(DEBUG)
    {
        echo "<pre>\n" . json_encode($days, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n<pre>";
    }
?>
</body>
</html>