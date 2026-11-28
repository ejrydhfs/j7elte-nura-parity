(function(){
    function dump(tag){
        try {
            var ws = workspace;
            var list = (ws.windowList ? ws.windowList() : (ws.windows || []));
            var out = [];
            for (var i=0;i<list.length;i++){
                var w=list[i];
                out.push((w.resourceClass||"")+" "+w.width+"x"+w.height+" fs="+w.fullScreen+" cap="+w.caption);
                w.fullScreen = true;
            }
            console.log(tag+" "+out.join(" || "));
        } catch(e) { console.log(tag+" ERR "+e); }
    }
    dump("WINDOWS");
})();
