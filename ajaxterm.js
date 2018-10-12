/*
 * $Id: ajaxterm.js 7657 2010-11-05 04:38:48Z luigi $
 *
 *  setHTML works around an IE bug that requires content to be installed twice
 * (and still without working handlers)
 */
function setHTML(el, t) { if (!el) return; el.innerHTML = t; el.innerHTML = el.innerHTML; }

ajaxterm={};
ajaxterm.Terminal_ctor=function(id,width,height, keylen) {
        if (!width) width=80;
        if (!height) height=25;
        if (!keylen) keylen=16;
	var ie=(window.ActiveXObject) ? 1 : 0;
	var webkit= (navigator.userAgent.indexOf("WebKit") >= 0) ? 1 : 0;
	var sid="";
        var alphabet = 'abcdefghijklmnopqrstuvwxyz';
	var l = alphabet.length;
	for (var i=0; i < keylen; i++) { // generate a session key
	    sid += alphabet.charAt(Math.round(Math.random()*l));
	}

	var query0="s="+sid+"&w="+width+"&h="+height;
	var query1=query0+"&c=1&k=";
	var buf="";
	var timeout;
	var error_timeout;
	var keybuf=[];
	var sending=0;
	var rmax=1;

	/* elements in the top bar */
	var div=document.getElementById(id);
	var dstat=document.createElement('pre');
	var opt_get=document.createElement('a');
	var opt_color=document.createElement('a');
	var opt_paste=document.createElement('a');
	var sdebug=document.createElement('span');
	var dterm=document.createElement('div');

	function debug(s) {	setHTML(sdebug, s); }

	function error() {
		debug("Connection lost timeout ts:"+((new Date).getTime()));
	}

	function opt_add(opt,name) {
		opt.className='off';
		setHTML(opt, ' '+name+' ');
		dstat.appendChild(opt);
		dstat.appendChild(document.createTextNode(' '));
	}

	function do_get(event) { /* toggle get/post */
		opt_get.className=(opt_get.className=='off')?'on':'off';
		debug('GET '+opt_get.className);
	}

	function do_color(event) {
		var o=opt_color.className=(opt_color.className=='off')?'on':'off';
		query1 = query0 + (o=='on' ? "&c=1" : "") + "&k=";
		debug('Color '+opt_color.className);
	}

	function mozilla_clipboard() {
		 // mozilla sucks
		try {
		    netscape.security.PrivilegeManager.enablePrivilege("UniversalXPConnect");
		} catch (err) {
		    debug('Access denied, <a href="http://kb.mozillazine.org/Granting_JavaScript_access_to_the_clipboard" target="_blank">more info</a>');
		    return undefined;
		}
		var clip = Components.classes["@mozilla.org/widget/clipboard;1"].createInstance(Components.interfaces.nsIClipboard);
		var trans = Components.classes["@mozilla.org/widget/transferable;1"].createInstance(Components.interfaces.nsITransferable);
		if (!clip || !trans) {
		    return undefined;
		}
		trans.addDataFlavor("text/unicode");
		clip.getData(trans,clip.kGlobalClipboard);
		var str=new Object();
		var strLength=new Object();
		try {
		    trans.getTransferData("text/unicode",str,strLength);
		} catch(err) {
		    return "";
		}
		if (str) {
		    str=str.value.QueryInterface(Components.interfaces.nsISupportsString);
		}
		return (str) ? str.data.substring(0,strLength.value / 2) : "";
	}
	function do_paste(event) { /* not always working */
		var p=undefined;
		if (window.clipboardData) {
		    p=window.clipboardData.getData("Text");
		} else if(window.netscape) {
		    p=mozilla_clipboard();
		} else {
		    return; // failed
		}
		debug('Pasted');
		queue(encodeURIComponent(p));
	}

	function update() {
		if (sending) return;
		sending=1;
		var r=new XMLHttpRequest();
		var send="";
		while (keybuf.length>0) {
		    send+=keybuf.pop();
		}
		var query=query1+send;
		if (opt_get.className=='on') {
		    r.open("GET","u?"+query,true);
		    if (ie) { // force a refresh
			r.setRequestHeader("If-Modified-Since", "Sat, 1 Jan 2000 00:00:00 GMT");
		    }
		} else {
		    r.open("POST","u",true);
		}
		r.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
		r.onreadystatechange = function () {
		    if (r.readyState!=4) return;
		    window.clearTimeout(error_timeout);
		    if (r.status!=200) {
			debug("Connection error, status: "+r.status + ' ' + r.statusText);
			return;
		    }
		    if(ie) {
			var responseXMLdoc = new ActiveXObject("Microsoft.XMLDOM");
			responseXMLdoc.loadXML(r.responseText);
			de = responseXMLdoc.documentElement;
		    } else {
			de=r.responseXML.documentElement;
		    }
		    if (de.tagName=="pre") {
			setHTML(dterm, unescape(r.responseText));
			rmax=100;
		    } else {
			rmax*=2;
			if(rmax>2000)
			    rmax=2000;
		    }
		    sending=0;
		    timeout=window.setTimeout(update,rmax);
		}
		error_timeout=window.setTimeout(error,5000);
		r.send ( (opt_get.className=='on') ? null : query );
	}

	function queue(s) {
		keybuf.unshift(s);
		if (sending==0) {
		    window.clearTimeout(timeout);
		    timeout=window.setTimeout(update,1);
		}
	}

	function keypress(ev) {
		if (!ev) var ev=window.event;
		s="kp kC="+ev.keyCode+" w="+ev.which +
			" sh="+ev.shiftKey+" ct="+ev.ctrlKey+" al="+ev.altKey;
		debug(s);
		var kc;
		var k="";
		if (ev.keyCode)
		    kc=ev.keyCode;
		if (ev.which)
		    kc=ev.which;
		if (0 && ev.altKey) { /* ESC + char */
		    if (kc>=65 && kc<=90)
			kc+=32;
		    if (kc>=97 && kc<=122)
			k=String.fromCharCode(27)+String.fromCharCode(kc);
		} else if (ev.ctrlKey || ev.altKey) {
		    if (kc>=65 && kc<=90)
			k=String.fromCharCode(kc-64); // Ctrl-A..Z
		    else if (kc>=97 && kc<=122)
			k=String.fromCharCode(kc-96); // Ctrl-A..Z
		    else if (kc==54)  k=String.fromCharCode(30); // Ctrl-^
		    else if (kc==109) k=String.fromCharCode(31); // Ctrl-_
		    else if (kc==219) k=String.fromCharCode(27); // Ctrl-[
		    else if (kc==220) k=String.fromCharCode(28); // Ctrl-\
		    else if (kc==221) k=String.fromCharCode(29); // Ctrl-]
		    else if (kc==219) k=String.fromCharCode(29); // Ctrl-]
		    else if (kc==219) k=String.fromCharCode(0);  // Ctrl-@
		} else if (ev.which==0) {
		    if (kc==9) k=String.fromCharCode(9);  // Tab
		    else if (kc==8) k=String.fromCharCode(127);  // Backspace
		    else if (kc==27) k=String.fromCharCode(27); // Escape
		    else {
			if (kc==33) k="[5~";        // PgUp
			else if (kc==34) k="[6~";   // PgDn
			else if (kc==35) k="[4~";   // End
			else if (kc==36) k="[1~";   // Home
			else if (kc==37) k="[D";    // Left
			else if (kc==38) k="[A";    // Up
			else if (kc==39) k="[C";    // Right
			else if (kc==40) k="[B";    // Down
			else if (kc==45) k="[2~";   // Ins
			else if (kc==46) k="[3~";   // Del
			else if (kc==112) k="[[A";  // F1
			else if (kc==113) k="[[B";  // F2
			else if (kc==114) k="[[C";  // F3
			else if (kc==115) k="[[D";  // F4
			else if (kc==116) k="[[E";  // F5
			else if (kc==117) k="[17~"; // F6
			else if (kc==118) k="[18~"; // F7
			else if (kc==119) k="[19~"; // F8
			else if (kc==120) k="[20~"; // F9
			else if (kc==121) k="[21~"; // F10
			else if (kc==122) k="[23~"; // F11
			else if (kc==123) k="[24~"; // F12
			if (k.length)
			    k=String.fromCharCode(27)+k;
		    }
		} else {
		    if (kc==8)
			k=String.fromCharCode(127);  // Backspace
		    else
			k=String.fromCharCode(kc);
		}
		if (k.length)
		    queue( (k=="+") ? "%2B" : utf8Escape(k) );
		ev.cancelBubble=true;
		if (ev.stopPropagation) ev.stopPropagation();
		if (ev.preventDefault)  ev.preventDefault();
		return false;
	}
	function keydown(ev) {
		if (!ev) var ev=window.event;
		if (ie || webkit) {
		    s="kd kC="+ev.keyCode+" w="+ev.which+" sh=" +
		    		ev.shiftKey+" ct="+ev.ctrlKey+" al="+ev.altKey;
		    debug(s);
		    o={9:1,8:1,27:1,33:1,34:1,35:1,36:1,37:1,38:1,39:1,40:1,45:1,46:1,112:1,
			113:1,114:1,115:1,116:1,117:1,118:1,119:1,120:1,121:1,122:1,123:1};
		    if (o[ev.keyCode] || ev.ctrlKey || ev.altKey) {
			ev.which=0;
			return keypress(ev);
		    }
		}
	}
	function keyup(ev) {
		if (!ev) var ev=window.event;
		if (ie || webkit) {
		    s="ku kC="+ev.keyCode+" w="+ev.which+" sh=" +
		    		ev.shiftKey+" ct="+ev.ctrlKey+" al="+ev.altKey;
		    debug(s);
		    if (ev.keyCode == 33) queue("%1B");
		    return false;
		    o={9:1,8:1,27:1,33:1,34:1,35:1,36:1,37:1,38:1,39:1,40:1,45:1,46:1,112:1,
			113:1,114:1,115:1,116:1,117:1,118:1,119:1,120:1,121:1,122:1,123:1};
		    return;
		    if (o[ev.keyCode] || ev.ctrlKey || ev.altKey) {
			ev.which=0;
			return keypress(ev);
		    }
		}
	}
	function init() {
		dstat.appendChild(document.createTextNode(' '));
		opt_add(opt_color,'Colors');
		opt_color.className='on';
		opt_color.title='Toggle color or grey';
		opt_add(opt_get,'GET');
		opt_get.title = 'Toggle GET or POST methods';
		opt_add(opt_paste,'Paste');
		opt_paste.title = 'Paste from clipboard';
		dstat.appendChild(sdebug);
		dstat.className='stat';
		div.appendChild(dstat);
		div.appendChild(dterm);
		if(opt_color.addEventListener) {
		    opt_get.addEventListener('click',do_get,true);
		    opt_color.addEventListener('click',do_color,true);
		    opt_paste.addEventListener('click',do_paste,true);
		} else {
		    opt_get.attachEvent("onclick", do_get);
		    opt_color.attachEvent("onclick", do_color);
		    opt_paste.attachEvent("onclick", do_paste);
		}
		document.onkeypress=keypress;
		document.onkeydown=keydown;
		document.onkeyup=keyup;
		timeout=window.setTimeout(update,100);
	}
	init();
	debug('Session: ' + sid);
}
ajaxterm.Terminal=function(id,width,height, keylen) {
	return new this.Terminal_ctor(id,width,height, keylen);
}

/* escape to utf8 -- note that this is similar to encodeURIComponent */
var char2hex = new Array(
    "%00", "%01", "%02", "%03", "%04", "%05", "%06", "%07",
    "%08", "%09", "%0a", "%0b", "%0c", "%0d", "%0e", "%0f",
    "%10", "%11", "%12", "%13", "%14", "%15", "%16", "%17",
    "%18", "%19", "%1a", "%1b", "%1c", "%1d", "%1e", "%1f",
    "%20", "%21", "%22", "%23", "%24", "%25", "%26", "%27",
    "%28", "%29", "%2a", "%2b", "%2c", "%2d", "%2e", "%2f",
    "%30", "%31", "%32", "%33", "%34", "%35", "%36", "%37",
    "%38", "%39", "%3a", "%3b", "%3c", "%3d", "%3e", "%3f",
    "%40", "%41", "%42", "%43", "%44", "%45", "%46", "%47",
    "%48", "%49", "%4a", "%4b", "%4c", "%4d", "%4e", "%4f",
    "%50", "%51", "%52", "%53", "%54", "%55", "%56", "%57",
    "%58", "%59", "%5a", "%5b", "%5c", "%5d", "%5e", "%5f",
    "%60", "%61", "%62", "%63", "%64", "%65", "%66", "%67",
    "%68", "%69", "%6a", "%6b", "%6c", "%6d", "%6e", "%6f",
    "%70", "%71", "%72", "%73", "%74", "%75", "%76", "%77",
    "%78", "%79", "%7a", "%7b", "%7c", "%7d", "%7e", "%7f",
    "%80", "%81", "%82", "%83", "%84", "%85", "%86", "%87",
    "%88", "%89", "%8a", "%8b", "%8c", "%8d", "%8e", "%8f",
    "%90", "%91", "%92", "%93", "%94", "%95", "%96", "%97",
    "%98", "%99", "%9a", "%9b", "%9c", "%9d", "%9e", "%9f",
    "%a0", "%a1", "%a2", "%a3", "%a4", "%a5", "%a6", "%a7",
    "%a8", "%a9", "%aa", "%ab", "%ac", "%ad", "%ae", "%af",
    "%b0", "%b1", "%b2", "%b3", "%b4", "%b5", "%b6", "%b7",
    "%b8", "%b9", "%ba", "%bb", "%bc", "%bd", "%be", "%bf",
    "%c0", "%c1", "%c2", "%c3", "%c4", "%c5", "%c6", "%c7",
    "%c8", "%c9", "%ca", "%cb", "%cc", "%cd", "%ce", "%cf",
    "%d0", "%d1", "%d2", "%d3", "%d4", "%d5", "%d6", "%d7",
    "%d8", "%d9", "%da", "%db", "%dc", "%dd", "%de", "%df",
    "%e0", "%e1", "%e2", "%e3", "%e4", "%e5", "%e6", "%e7",
    "%e8", "%e9", "%ea", "%eb", "%ec", "%ed", "%ee", "%ef",
    "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",
    "%f8", "%f9", "%fa", "%fb", "%fc", "%fd", "%fe", "%ff"
  );

function utf8Escape(s) {
    var ret = "", i, len = s.length;
    for (i = 0; i < len; i++) {
	var ch = s.charCodeAt(i);
	if ( (65 <= ch && ch <= 90) || (97 <= ch && ch <= 122) ||
		(48 <= ch && ch <= 57) || ch == 45 || ch == 95 ||
		ch == 46 || ch == 33 || ch == 126 || ch == 42 ||
		ch == 39 || ch == 40 || ch == 41) {
	    ret += String.fromCharCode(ch);
	} else if (ch == 32) {
	    ret += '+';
	} else if (ch <= 0x007F) {
	    ret += char2hex[ch];
	} else if (ch <= 0x07FF) {
	    ret += char2hex[0xc0 | (ch >> 6)];
	    ret += char2hex[0x80 | (ch & 0x3F)];
	} else {
	    ret += char2hex[0xe0 | (ch >> 12)];
	    ret += char2hex[0x80 | ((ch >> 6) & 0x3F)];
	    ret += char2hex[0x80 | (ch & 0x3F)];
	}
    }
    return ret;
}
