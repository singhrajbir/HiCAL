from config.settings.base import DOCUMENTS_URL
from config.settings.base import PARA_URL

import httplib2

try:
    # For c speedups
    from simplejson import loads
except ImportError:
    from json import loads


def get_date(content):
    for line in content.split('\n'):
        if line.strip()[:4] == "Sent":
            return line.split(':', 1)[1].strip()
    return ""


def get_subject(content):
    for line in content.split('\n'):
        if line.strip()[:7] == "Subject":
            return line.split(':', 1)[1].strip()
    return ""

def highlight_color_map(top_terms):
    scores = [v for k,v in top_terms.items()]
    min_score = min(scores)
    #colors = ["#FFFAC8", "#FFFA96", "#FFFA64", "#FFFA19"]  # yellows
    colors = ["#FFECCC", "#FFDA99", "#FFC766", "#FFB533"]   # oranges
    notch = (max(scores) - min(scores))/len(colors)

    result = {}
    for term, score in top_terms.items():
        if score < min_score + notch:
            result[term] = colors[0]
        elif score < min_score + 2*notch:
            result[term] = colors[1]
        elif score < min_score + 3*notch:
            result[term] = colors[2]
        else:
            result[term] = colors[3]
    return result

def add_mark_tags(word, top_terms, color_map):
    for term in top_terms:
        if term in word.lower() and len(term) > 1:
            term_score = term + " : " + str(top_terms[term])[:5]    # Display score to three decimal places
            color = color_map[term]
            return "<span data-title=\"{0}\"><mark style=\"background-color:{2};\">{1}</mark></span>".format(term_score, word, color)
    return word

def add_highlighting(content, top_terms):
    color_map = highlight_color_map(top_terms)
    content_result = []
    content_words = content.split(" ")
    for word in content_words:
        if "<br/>" in word:
            parts_result = []
            parts = word.split("<br/>")
            for part in parts:
                parts_result.append(add_mark_tags(part, top_terms, color_map))
            content_result.append("<br/>".join(parts_result))
        else:
            content_result.append(add_mark_tags(word, top_terms, color_map))

    return " ".join(content_result)

def get_documents(doc_ids, query=None):
    """
    :param query:
    :param doc_ids: the ids of documents to return
    :return: documents content
    """
    result = []
    h = httplib2.Http()
    for doc_id in doc_ids:
        url = '{}/{}'.format(DOCUMENTS_URL, doc_id)
        resp, content = h.request(url,
                                  method="GET")
        content = content.decode('utf-8', 'ignore')
        date = get_date(content)
        title = get_subject(content)
        if len(content) == 0:
            if len(title) == 0:
                title = '<i class="text-warning">The document title is empty</i>'
            content = '<i class="text-warning">The document content is empty</i>'
        else:
            if len(title) == 0:
                title = content[:32]

        document = {
            'doc_id': doc_id,
            'title': title,
            'content': content.replace("\n", "<br/>"),
            'date': date
        }
        result.append(document)

    return result


def get_documents_with_snippet(doc_ids, query):
    h = httplib2.Http()
    url = "{}/{}"
    doc_ids_unique = []
    doc_ids_set = set()
    for doc_id in doc_ids:
        if doc_id['doc_id'] not in doc_ids_set:
            doc_ids_set.add(doc_id['doc_id'])
            doc_ids_unique.append(doc_id)

    doc_ids = doc_ids_unique

    result = get_documents([doc['doc_id'] for doc in doc_ids], query)
    for doc_para_id, doc in zip(doc_ids, result):
        if 'para_id' not in doc_para_id:
            doc['snippet'] = u'N/A'
            continue
        try:
            para_id = doc_para_id['doc_id'] + '.' + doc_para_id['para_id']
            resp, content = h.request(url.format(PARA_URL, para_id),
                                      method="GET")
            doc['snippet'] = content.decode('utf-8', 'ignore').replace("\n", "<br />");
        except:
            doc['snippet'] = u'N/A'
    return result
