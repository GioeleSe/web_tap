const canvas = document.getElementById('videoCanvas');
const ctx = canvas.getContext('2d');

const wsProtocol = window.location.protocol === 'https:'?'wss://':'ws://';
const ws = new WebSocket(wsProtocol + window.location.host + '/ws/video');
ws.binaryType = 'arraybuffer';

ws.onmessage = async function(event){
    const blob = new Blob([event.data], {type:'image/jpeg'});
    try{
        const bitmap = await createImageBitmap(blob);
        ctx.clearRect(0,0, canvas.width, canvas.height);
        ctx.drawImage(bitmap, 0, 0);
    }catch(err){
        console.error("frame render error: ", err);
    }
};

ws.onclose = function(){
    console.log("Video ws disconnected. Reloading in 2s");
    setTimeout(() => {
        location.reload()
    }, 2000);
};